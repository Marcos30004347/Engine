#include "sparse_voxel_brick_common.wgsl"

#ifndef TERRAIN_V2_FINE_RASTER_WORKGROUP
#define TERRAIN_V2_FINE_RASTER_WORKGROUP 64
#endif

const TERRAIN_V2_FINE_RASTER_STATUS_BUILDING : u32 = 1u;
const TERRAIN_V2_FINE_RASTER_STATUS_READY : u32 = 2u;
const TERRAIN_V2_FINE_RASTER_STATUS_OVERFLOW : u32 = 3u;
const TERRAIN_V2_FINE_RASTER_STATUS_INVALID : u32 = 4u;
const TERRAIN_V2_FINE_RASTER_STATUS_STALE_COLUMNS : u32 = 5u;
const TERRAIN_V2_FINE_RASTER_STATUS_VALIDATION_FAILED : u32 = 6u;
const TERRAIN_V2_COLUMN_STATUS_READY : u32 = 2u;
const TERRAIN_V2_FINE_RASTER_FLAGS : u32 = 7u;

struct TerrainGPUV2FineRasterConfig {
    bricksX : u32,
    bricksY : u32,
    bricksZ : u32,
    brickSide : u32,
    columnsX : u32,
    columnsZ : u32,
    columnStride : u32,
    halo : u32,
    denseBrickCount : u32,
    nodeCapacity : u32,
    materialCapacity : u32,
    materialWordCapacity : u32,
    canonicalStep : u32,
    chunkMinimumCanonicalX : i32,
    chunkMinimumCanonicalY : i32,
    chunkMinimumCanonicalZ : i32,
    chunkMinimumWorldX : f32,
    chunkMinimumWorldY : f32,
    chunkMinimumWorldZ : f32,
    voxelSizeMetres : f32,
    expectedTerrainGeneration : u32,
    regionMappingId : u32,
    paletteBase : u32,
    generatorRevision : u32,
    wordBase : u32,
    countBase : u32,
    offsetBase : u32,
    level0SumsBase : u32,
    level0OffsetsBase : u32,
    level1SumsBase : u32,
    level1OffsetsBase : u32,
    compactBase : u32,
    surfaceShellDepthVoxels : u32,
    boundaryStitchSubdivisions : u32,
    reserved0 : u32,
    reserved1 : u32,
}

struct TerrainGPUV2Column {
    minimumHeightUnits : i32,
    maximumHeightUnits : i32,
    material : u32,
    flags : u32,
}

struct TerrainGPUV2ColumnResult {
    status : u32,
    missingSamples : u32,
    faultCount : u32,
    quantizationOverflows : u32,
    heightSampleCount : u32,
    columnCount : u32,
    terrainGeneration : u32,
    writtenColumns : u32,
    atlasRevisionLow : u32,
    atlasRevisionHigh : u32,
    generatorRevision : u32,
    selectedWorkgroup : u32,
    reserved0 : u32,
    reserved1 : u32,
    reserved2 : u32,
    reserved3 : u32,
}

@group(0) @binding(0) var<storage, read> configBuffer : array<TerrainGPUV2FineRasterConfig>;
@group(0) @binding(1) var<storage, read> columns : array<TerrainGPUV2Column>;
@group(0) @binding(2) var<storage, read> columnResultBuffer : array<TerrainGPUV2ColumnResult>;
@group(0) @binding(3) var<storage, read_write> scratch : array<u32>;
@group(0) @binding(4) var<storage, read_write> nodes : array<SparseVoxelBrickNode>;
@group(0) @binding(5) var<storage, read_write> occupancies : array<SparseVoxelBrickOccupancy>;
@group(0) @binding(6) var<storage, read_write> materialWords : array<atomic<u32>>;
@group(0) @binding(7) var<storage, read_write> rootIndices : array<u32>;
@group(0) @binding(8) var<storage, read_write> result : array<atomic<u32>>;

var<workgroup> sharedNode : array<u32, 256>;
var<workgroup> sharedMaterial : array<u32, 256>;
var<private> laneIndex : u32;

fn config() -> TerrainGPUV2FineRasterConfig {
    return configBuffer[laneIndex];
}

fn scratch_base() -> u32 {
    return laneIndex * config().reserved1;
}
fn node_base() -> u32 { return laneIndex * config().reserved0; }
fn material_base() -> u32 {
    return laneIndex * config().materialWordCapacity;
}
fn column_base() -> u32 {
    return laneIndex * columnResultBuffer[laneIndex].reserved3;
}
fn result_base() -> u32 { return laneIndex * 16u; }

fn pairLoad(base : u32, index : u32) -> vec2<u32> {
    return vec2<u32>(scratch[scratch_base() + base + index * 2u], scratch[scratch_base() + base + index * 2u + 1u]);
}

fn pairStore(base : u32, index : u32, value : vec2<u32>) {
    scratch[scratch_base() + base + index * 2u] = value.x;
    scratch[scratch_base() + base + index * 2u + 1u] = value.y;
}

fn wordAddress(brick : u32, word : u32) -> u32 {
    return config().wordBase + (brick * 16u + word) * 8u;
}

fn brickCoordinate(index : u32) -> vec3<u32> {
    let c = config();
    let yz = c.bricksY * c.bricksZ;
    let x = index / yz;
    let remainder = index - x * yz;
    let y = remainder / c.bricksZ;
    return vec3<u32>(x, y, remainder - y * c.bricksZ);
}

fn floorDivide(numerator : i32, denominator : i32) -> i32 {
    let quotient = numerator / denominator;
    let remainder = numerator % denominator;
    return quotient - select(0, 1, remainder < 0);
}

// The source-map pass resolves a per-page vertical datum entirely on the GPU.
// Keeping one resident cell below its aligned minimum provides the exterior
// shell closure without tying fine LODs to a zero-metre vertical window.
fn resolvedChunkMinimumCanonicalY() -> i32 {
    if (columnResultBuffer[laneIndex].reserved2 != 1u) {
        return config().chunkMinimumCanonicalY;
    }
    let step = i32(config().canonicalStep);
    let minimumHeight = bitcast<i32>(columnResultBuffer[laneIndex].reserved0);
    return floorDivide(minimumHeight, step) * step - step;
}

fn resolvedChunkMinimumWorldY() -> f32 {
    let c = config();
    if (columnResultBuffer[laneIndex].reserved2 != 1u) {
        return c.chunkMinimumWorldY;
    }
    return f32(resolvedChunkMinimumCanonicalY()) *
        (c.voxelSizeMetres / f32(c.canonicalStep));
}

fn scanCount(level : u32) -> u32 {
    let dense = config().denseBrickCount;
    if (level == 0u) { return dense; }
    let level0 = (dense + 255u) / 256u;
    if (level == 1u) { return level0; }
    return (level0 + 255u) / 256u;
}

fn scanInputBase(level : u32) -> u32 {
    let c = config();
    if (level == 0u) { return c.countBase; }
    if (level == 1u) { return c.level0SumsBase; }
    return c.level1SumsBase;
}

fn scanOutputBase(level : u32) -> u32 {
    let c = config();
    if (level == 0u) { return c.offsetBase; }
    if (level == 1u) { return c.level0OffsetsBase; }
    return c.level1OffsetsBase;
}

fn scanSumBase(level : u32) -> u32 {
    let c = config();
    if (level == 0u) { return c.level0SumsBase; }
    return c.level1SumsBase;
}

fn runScan(level : u32, local : u32, group : u32) {
    let count = scanCount(level);
    let index = group * 256u + local;
    var value = vec2<u32>(0u);
    if (index < count) {
        value = pairLoad(scanInputBase(level), index);
    }
    sharedNode[local] = value.x;
    sharedMaterial[local] = value.y;
    workgroupBarrier();
    var stride = 1u;
    while (stride < 256u) {
        let destination = (local + 1u) * stride * 2u - 1u;
        if (destination < 256u) {
            sharedNode[destination] += sharedNode[destination - stride];
            sharedMaterial[destination] += sharedMaterial[destination - stride];
        }
        stride *= 2u;
        workgroupBarrier();
    }
    if (local == 255u) {
        pairStore(scanSumBase(level), group,
            vec2<u32>(sharedNode[255u], sharedMaterial[255u]));
        sharedNode[255u] = 0u;
        sharedMaterial[255u] = 0u;
    }
    workgroupBarrier();
    stride = 128u;
    while (stride > 0u) {
        let destination = (local + 1u) * stride * 2u - 1u;
        if (destination < 256u) {
            let leftNode = sharedNode[destination - stride];
            let leftMaterial = sharedMaterial[destination - stride];
            sharedNode[destination - stride] = sharedNode[destination];
            sharedMaterial[destination - stride] = sharedMaterial[destination];
            sharedNode[destination] += leftNode;
            sharedMaterial[destination] += leftMaterial;
        }
        stride /= 2u;
        workgroupBarrier();
    }
    if (index < count) {
        pairStore(scanOutputBase(level), index,
            vec2<u32>(sharedNode[local], sharedMaterial[local]));
    }
}

fn addScanLevel(destinationBase : u32, parentBase : u32,
    count : u32, index : u32) {
    if (index < count) {
        pairStore(destinationBase, index,
            pairLoad(destinationBase, index) + pairLoad(parentBase, index / 256u));
    }
}

fn replaceMaterialByte(address : u32, value : u32) {
    let word = address >> 2u;
    let shift = (address & 3u) * 8u;
    let mask = 0xffu << shift;
    var old = atomicLoad(&materialWords[material_base() + word]);
    loop {
        let replacement = (old & ~mask) | ((value & 0xffu) << shift);
        let exchange = atomicCompareExchangeWeak(&materialWords[material_base() + word], old, replacement);
        if (exchange.exchanged) { break; }
        old = exchange.old_value;
    }
}

@compute @workgroup_size(1, 1, 1)
fn resetTerrainGPUV2FineRaster(@builtin(global_invocation_id) gid : vec3<u32>) {
    laneIndex = gid.z;
    let c = config();
    let resolvedMinimumY = resolvedChunkMinimumWorldY();
    atomicStore(&result[result_base() + 0], TERRAIN_V2_FINE_RASTER_STATUS_BUILDING);
    atomicStore(&result[result_base() + 1], 0u);
    atomicStore(&result[result_base() + 2], 0u);
    atomicStore(&result[result_base() + 3], TERRAIN_V2_FINE_RASTER_FLAGS);
    atomicStore(&result[result_base() + 4], c.denseBrickCount);
    atomicStore(&result[result_base() + 5], 0u);
    atomicStore(&result[result_base() + 6], 0u);
    atomicStore(&result[result_base() + 7], 0u);
    atomicStore(&result[result_base() + 8], 0u);
    atomicStore(&result[result_base() + 9], c.expectedTerrainGeneration);
    atomicStore(&result[result_base() + 10], c.generatorRevision);
    atomicStore(&result[result_base() + 11], TERRAIN_V2_FINE_RASTER_WORKGROUP);
    atomicStore(&result[result_base() + 12], c.compactBase + c.denseBrickCount);
    atomicStore(&result[result_base() + 13], 0u);
    atomicStore(&result[result_base() + 14], 0u);
    // The renderer ABI carries this offset through hierarchy adaptation and
    // joint publication, so raster nodes, Tree64 bounds and dynamic instances
    // share the same GPU-resolved datum.
    atomicStore(&result[result_base() + 15], bitcast<u32>(resolvedMinimumY));
    if (c.brickSide != 8u || c.regionMappingId == 0u || c.denseBrickCount == 0u) {
        atomicStore(&result[result_base() + 0], TERRAIN_V2_FINE_RASTER_STATUS_INVALID);
    } else if (columnResultBuffer[laneIndex].reserved2 == 1u) {
        let step = i32(c.canonicalStep);
        let sourceMinimum = bitcast<i32>(
            columnResultBuffer[laneIndex].reserved0);
        let sourceMaximum = bitcast<i32>(
            columnResultBuffer[laneIndex].reserved1);
        let upperExclusive =
            -floorDivide(-sourceMaximum, step) * step;
        let capacityExclusive = resolvedChunkMinimumCanonicalY() +
            i32(c.bricksY * c.brickSide) * step;
        if (sourceMaximum < sourceMinimum ||
            upperExclusive > capacityExclusive) {
            // Never publish a raster/Tree64 pair which agrees only because
            // both silently discarded the top of a tall source page.
            atomicStore(&result[result_base() + 0],
                TERRAIN_V2_FINE_RASTER_STATUS_OVERFLOW);
            atomicStore(&result[result_base() + 1], 3u);
        }
    }
}

@compute @workgroup_size(TERRAIN_V2_FINE_RASTER_WORKGROUP, 1, 1)
fn classifyTerrainGPUV2FineRaster(@builtin(global_invocation_id) gid : vec3<u32>) {
    laneIndex = gid.z;
    let c = config();
    let item = gid.x;
    if (atomicLoad(&result[result_base() + 0]) !=
            TERRAIN_V2_FINE_RASTER_STATUS_BUILDING ||
        item >= c.denseBrickCount * 16u ||
        columnResultBuffer[laneIndex].status != TERRAIN_V2_COLUMN_STATUS_READY ||
        columnResultBuffer[laneIndex].terrainGeneration != c.expectedTerrainGeneration) {
        return;
    }
    let brickIndex = item / 16u;
    let word = item - brickIndex * 16u;
    let brick = brickCoordinate(brickIndex);
    var bits = 0u;
    var count = 0u;
    var minimum = vec3<u32>(7u);
    var maximum = vec3<u32>(0u);
    var coarseLow = 0u;
    var coarseHigh = 0u;
    for (var bit = 0u; bit < 32u; bit += 1u) {
        let cell = word * 32u + bit;
        let local = vec3<u32>(cell & 7u, (cell >> 3u) & 7u, cell >> 6u);
        let voxel = brick * 8u + local;
        let columnAddress = (voxel.z + c.halo) * c.columnStride + voxel.x + c.halo;
        let column = columns[column_base() + columnAddress];
        if (column.material > 255u) {
            atomicOr(&result[result_base() + 2], 1u);
            continue;
        }
        let chunkMinimumCanonicalY = resolvedChunkMinimumCanonicalY();
        let canonicalY = chunkMinimumCanonicalY +
            i32(voxel.y * c.canonicalStep);
        // Use the same half-open surface ownership as CPU terrain generation.
        // A lower corner on a voxel boundary belongs to that voxel, not to an
        // additional voxel below it. A flat cell still owns the cell directly
        // below the shared min/max surface boundary.
        let step = i32(c.canonicalStep);
        let relativeMinimum = column.minimumHeightUnits -
            chunkMinimumCanonicalY;
        let relativeMaximum = column.maximumHeightUnits -
            chunkMinimumCanonicalY;
        let roundedSurfaceFloor = chunkMinimumCanonicalY +
            floorDivide(relativeMinimum, step) * step;
        let surfaceTop = chunkMinimumCanonicalY +
            (-floorDivide(-relativeMaximum, step) - 1) * step;
        // A perfectly flat surface exactly on a lattice plane has
        // roundedSurfaceFloor == plane and surfaceTop == plane - step. The
        // half-open ownership rule assigns that surface to the cell below;
        // leaving the interval inverted drops the voxel from both the raster
        // and the byte-identical Tree64 payload.
        let surfaceFloor = min(roundedSurfaceFloor, surfaceTop);
        // Close exposed side faces against the four neighbouring columns.
        // Columns carry a halo, so the rule is continuous across page edges.
        var lowestNeighbour = surfaceTop;
        let signedColumn = vec2<i32>(i32(voxel.x + c.halo),
            i32(voxel.z + c.halo));
        let neighbourOffsets = array<vec2<i32>, 4>(
            vec2<i32>(-1, 0), vec2<i32>(1, 0),
            vec2<i32>(0, -1), vec2<i32>(0, 1));
        for (var neighbourIndex = 0u; neighbourIndex < 4u;
             neighbourIndex += 1u) {
            let neighbour = signedColumn + neighbourOffsets[neighbourIndex];
            if (neighbour.x < 0 || neighbour.y < 0 ||
                neighbour.x >= i32(c.columnStride)) {
                continue;
            }
            let neighbourAddress = u32(neighbour.y) * c.columnStride +
                u32(neighbour.x);
            let neighbourColumn = columns[column_base() + neighbourAddress];
            if ((neighbourColumn.flags & 1u) == 0u) {
                continue;
            }
            let neighbourRelativeMaximum =
                neighbourColumn.maximumHeightUnits - chunkMinimumCanonicalY;
            let neighbourTop = chunkMinimumCanonicalY +
                (-floorDivide(-neighbourRelativeMaximum, step) - 1) * step;
            lowestNeighbour = min(lowestNeighbour, neighbourTop);
        }
        let firstExposedSide = select(surfaceTop,
            lowestNeighbour + step, lowestNeighbour < surfaceTop);
        let closure = i32(c.surfaceShellDepthVoxels) * step;
        let cliffClosureFloor = max(chunkMinimumCanonicalY,
            firstExposedSide - closure);
        let shellMinimum = min(surfaceFloor, cliffClosureFloor);
        if ((column.flags & 1u) == 0u || canonicalY < shellMinimum ||
            canonicalY > surfaceTop) {
            continue;
        }
        bits |= 1u << bit;
        count += 1u;
        minimum = min(minimum, local);
        maximum = max(maximum, local);
        let coarse = (local.x >> 1u) + 4u * ((local.y >> 1u) + 4u * (local.z >> 1u));
        if (coarse < 32u) {
            coarseLow |= 1u << coarse;
        } else {
            coarseHigh |= 1u << (coarse - 32u);
        }
    }
    let address = wordAddress(brickIndex, word);
    scratch[scratch_base() + address] = bits;
    scratch[scratch_base() + address + 1u] = count;
    scratch[scratch_base() + address + 2u] = select(0xffffffffu,
        minimum.x | (minimum.y << 8u) | (minimum.z << 16u), count != 0u);
    scratch[scratch_base() + address + 3u] = maximum.x | (maximum.y << 8u) | (maximum.z << 16u);
    scratch[scratch_base() + address + 4u] = coarseLow;
    scratch[scratch_base() + address + 5u] = coarseHigh;
    scratch[scratch_base() + address + 6u] = 0u;
    scratch[scratch_base() + address + 7u] = 0u;
}

@compute @workgroup_size(TERRAIN_V2_FINE_RASTER_WORKGROUP, 1, 1)
fn reduceTerrainGPUV2FineRaster(@builtin(global_invocation_id) gid : vec3<u32>) {
    laneIndex = gid.z;
    let c = config();
    if (gid.x >= c.denseBrickCount) { return; }
    var occupied = 0u;
    for (var word = 0u; word < 16u; word += 1u) {
        occupied += scratch[scratch_base() + wordAddress(gid.x, word) + 1u];
    }
    pairStore(c.countBase, gid.x, vec2<u32>(select(0u, 1u, occupied != 0u), occupied));
}

@compute @workgroup_size(256, 1, 1)
fn scanTerrainGPUV2FineRasterLevel0(
    @builtin(local_invocation_id) lid : vec3<u32>,
    @builtin(workgroup_id) wid : vec3<u32>) {
    laneIndex = wid.z;
    runScan(0u, lid.x, wid.x);
}

@compute @workgroup_size(256, 1, 1)
fn scanTerrainGPUV2FineRasterLevel1(
    @builtin(local_invocation_id) lid : vec3<u32>,
    @builtin(workgroup_id) wid : vec3<u32>) {
    laneIndex = wid.z;
    runScan(1u, lid.x, wid.x);
}

@compute @workgroup_size(256, 1, 1)
fn scanTerrainGPUV2FineRasterLevel2(
    @builtin(local_invocation_id) lid : vec3<u32>,
    @builtin(workgroup_id) wid : vec3<u32>) {
    laneIndex = wid.z;
    runScan(2u, lid.x, wid.x);
}

@compute @workgroup_size(256, 1, 1)
fn addTerrainGPUV2FineRasterLevel1(@builtin(global_invocation_id) gid : vec3<u32>) {
    laneIndex = gid.z;
    let c = config();
    let count = (c.denseBrickCount + 255u) / 256u;
    addScanLevel(c.level0OffsetsBase, c.level1OffsetsBase, count, gid.x);
}

@compute @workgroup_size(256, 1, 1)
fn addTerrainGPUV2FineRasterLevel0(@builtin(global_invocation_id) gid : vec3<u32>) {
    laneIndex = gid.z;
    let c = config();
    addScanLevel(c.offsetBase, c.level0OffsetsBase, c.denseBrickCount, gid.x);
}

@compute @workgroup_size(1, 1, 1)
fn validateTerrainGPUV2FineRaster(@builtin(global_invocation_id) gid : vec3<u32>) {
    laneIndex = gid.z;
    let c = config();
    if (atomicLoad(&result[result_base() + 0]) != TERRAIN_V2_FINE_RASTER_STATUS_BUILDING) { return; }
    if (columnResultBuffer[laneIndex].status != TERRAIN_V2_COLUMN_STATUS_READY ||
        columnResultBuffer[laneIndex].terrainGeneration != c.expectedTerrainGeneration) {
        atomicStore(&result[result_base() + 0], TERRAIN_V2_FINE_RASTER_STATUS_STALE_COLUMNS);
        return;
    }
    if (atomicLoad(&result[result_base() + 2]) != 0u) {
        atomicStore(&result[result_base() + 0], TERRAIN_V2_FINE_RASTER_STATUS_VALIDATION_FAILED);
        return;
    }
    let last = c.denseBrickCount - 1u;
    let totals = pairLoad(c.offsetBase, last) + pairLoad(c.countBase, last);
    if (totals.x > c.nodeCapacity) {
        atomicStore(&result[result_base() + 0], TERRAIN_V2_FINE_RASTER_STATUS_OVERFLOW);
        atomicStore(&result[result_base() + 1], 1u);
        return;
    }
    if (totals.y > c.materialCapacity) {
        atomicStore(&result[result_base() + 0], TERRAIN_V2_FINE_RASTER_STATUS_OVERFLOW);
        atomicStore(&result[result_base() + 1], 2u);
        return;
    }
    atomicStore(&result[result_base() + 5], totals.x);
    atomicStore(&result[result_base() + 6], totals.y);
    atomicStore(&result[result_base() + 7], totals.x);
    atomicStore(&result[result_base() + 8], totals.y);
}

@compute @workgroup_size(TERRAIN_V2_FINE_RASTER_WORKGROUP, 1, 1)
fn compactTerrainGPUV2FineRaster(@builtin(global_invocation_id) gid : vec3<u32>) {
    laneIndex = gid.z;
    let c = config();
    if (gid.x >= c.denseBrickCount || atomicLoad(&result[result_base() + 0]) != TERRAIN_V2_FINE_RASTER_STATUS_BUILDING) { return; }
    if (pairLoad(c.countBase, gid.x).x != 0u) {
        scratch[scratch_base() + c.compactBase + pairLoad(c.offsetBase, gid.x).x] = gid.x;
    }
}

@compute @workgroup_size(TERRAIN_V2_FINE_RASTER_WORKGROUP, 1, 1)
fn emitTerrainGPUV2FineRasterNodes(@builtin(global_invocation_id) gid : vec3<u32>) {
    laneIndex = gid.z;
    let c = config();
    let nodeIndex = gid.x;
    let nodeCount = atomicLoad(&result[result_base() + 5]);
    if (nodeIndex >= nodeCount || atomicLoad(&result[result_base() + 0]) != TERRAIN_V2_FINE_RASTER_STATUS_BUILDING) { return; }
    let dense = scratch[scratch_base() + c.compactBase + nodeIndex];
    let brick = brickCoordinate(dense);
    var minimum = vec3<u32>(7u);
    var maximum = vec3<u32>(0u);
    var coarseLow = 0u;
    var coarseHigh = 0u;
    for (var word = 0u; word < 16u; word += 1u) {
        let address = wordAddress(dense, word);
        if (scratch[scratch_base() + address + 1u] != 0u) {
            let packedMinimum = scratch[scratch_base() + address + 2u];
            let packedMaximum = scratch[scratch_base() + address + 3u];
            minimum = min(minimum, vec3<u32>(packedMinimum & 0xffu,
                (packedMinimum >> 8u) & 0xffu, (packedMinimum >> 16u) & 0xffu));
            maximum = max(maximum, vec3<u32>(packedMaximum & 0xffu,
                (packedMaximum >> 8u) & 0xffu, (packedMaximum >> 16u) & 0xffu));
        }
        coarseLow |= scratch[scratch_base() + address + 4u];
        coarseHigh |= scratch[scratch_base() + address + 5u];
    }
    let offsets = pairLoad(c.offsetBase, dense);
    let counts = pairLoad(c.countBase, dense);
    var node : SparseVoxelBrickNode;
    node.minimumVoxelSize = vec4<f32>(
        c.chunkMinimumWorldX + f32(brick.x * 8u) * c.voxelSizeMetres,
        resolvedChunkMinimumWorldY() +
            f32(brick.y * 8u) * c.voxelSizeMetres,
        c.chunkMinimumWorldZ + f32(brick.z * 8u) * c.voxelSizeMetres,
        c.voxelSizeMetres);
    node.packedOccupiedBounds = minimum.x | (minimum.y << 3u) |
        (minimum.z << 6u) | (maximum.x << 9u) | (maximum.y << 12u) |
        (maximum.z << 15u);
    node.coarseOccupancyLow = coarseLow;
    node.coarseOccupancyHigh = coarseHigh;
    node.occupancyIndex = nodeIndex;
    node.materialBase = offsets.y;
    node.parentIndex = SPARSE_VOXEL_BRICK_INVALID_INDEX;
    node.childCount = 0u;
    var clipmapLOD = 0u;
    var step = c.canonicalStep;
    while (step > 1u) {
        step >>= 1u;
        clipmapLOD += 1u;
    }
    node.level = SPARSE_VOXEL_BRICK_FORCE_LEAF_RASTER | (clipmapLOD << 8u);
    node.occupiedCount = counts.y;
    node.firstChild = 0u;
    node.instanceIndex = SPARSE_VOXEL_BRICK_INVALID_INDEX;
    node.paletteBase = c.paletteBase;
    node.regionMappingId = c.regionMappingId;
    node.selfError = c.voxelSizeMetres * 0.25;
    node.parentError = bitcast<f32>(0x7f7fffffu);
    node.reservedTailPadding = 0u;
    node.canonicalGridOriginScale = vec4<i32>(
        c.chunkMinimumCanonicalX + i32(brick.x * 8u * c.canonicalStep),
        resolvedChunkMinimumCanonicalY() +
            i32(brick.y * 8u * c.canonicalStep),
        c.chunkMinimumCanonicalZ + i32(brick.z * 8u * c.canonicalStep),
        i32(c.canonicalStep));
    nodes[node_base() + nodeIndex] = node;
    rootIndices[node_base() + nodeIndex] = nodeIndex;
}

@compute @workgroup_size(TERRAIN_V2_FINE_RASTER_WORKGROUP, 1, 1)
fn emitTerrainGPUV2FineRasterWords(@builtin(global_invocation_id) gid : vec3<u32>) {
    laneIndex = gid.z;
    let c = config();
    let item = gid.x;
    let nodeIndex = item / 16u;
    let word = item - nodeIndex * 16u;
    let nodeCount = atomicLoad(&result[result_base() + 5]);
    if (nodeIndex >= nodeCount || atomicLoad(&result[result_base() + 0]) != TERRAIN_V2_FINE_RASTER_STATUS_BUILDING) { return; }
    let dense = scratch[scratch_base() + c.compactBase + nodeIndex];
    let address = wordAddress(dense, word);
    let bits = scratch[scratch_base() + address];
    var localPrefix = 0u;
    for (var prior = 0u; prior < word; prior += 1u) {
        localPrefix += scratch[scratch_base() + wordAddress(dense, prior) + 1u];
    }
    let occupancyCellAddress = node_base() + nodeIndex;
    occupancies[occupancyCellAddress].cells[word].bits = bits;
    occupancies[occupancyCellAddress].cells[word].prefix = localPrefix;
    let brick = brickCoordinate(dense);
    var materialAddress = pairLoad(c.offsetBase, dense).y + localPrefix;
    for (var bit = 0u; bit < 32u; bit += 1u) {
        if ((bits & (1u << bit)) == 0u) { continue; }
        let cell = word * 32u + bit;
        let local = vec3<u32>(cell & 7u, (cell >> 3u) & 7u, cell >> 6u);
        let voxel = brick * 8u + local;
        let columnAddress = (voxel.z + c.halo) * c.columnStride + voxel.x + c.halo;
        replaceMaterialByte(materialAddress, columns[column_base() + columnAddress].material);
        materialAddress += 1u;
    }
}

@compute @workgroup_size(1, 1, 1)
fn finalizeTerrainGPUV2FineRaster(@builtin(global_invocation_id) gid : vec3<u32>) {
    laneIndex = gid.z;
    if (atomicLoad(&result[result_base() + 0]) == TERRAIN_V2_FINE_RASTER_STATUS_BUILDING) {
        atomicStore(&result[result_base() + 0], TERRAIN_V2_FINE_RASTER_STATUS_READY);
    }
}
