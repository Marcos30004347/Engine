#include "sparse_voxel_occupancy_common.wgsl"

struct SparseVoxelOccupancyDDARay {
    originMaximumDistance : vec4<f32>,
    direction : vec4<f32>,
}

struct SparseVoxelOccupancyDDAResult {
    globalCell : vec3<i32>,
    status : u32,
    distance : f32,
    skippedDistance : f32,
    queryCount : u32,
    baseCellSteps : u32,
    highestLevel : u32,
    contentGeneration : u32,
    reserved0 : u32,
    reserved1 : u32,
}

struct SparseVoxelOccupancyDDADispatch {
    rayCount : u32,
    maximumQueries : u32,
    levelCapacity : u32,
    reserved0 : u32,
}

struct SparseVoxelOccupancyDDAInterval {
    entry : f32,
    exit : f32,
    valid : u32,
}

struct SparseVoxelOccupancyDDACell {
    cell : vec3<i32>,
    valid : u32,
}

@group(0) @binding(0) var<storage, read> occupancyState : SparseVoxelOccupancyVolume;
@group(0) @binding(1) var<uniform> pyramidState : SparseVoxelOccupancyPyramid;
@group(0) @binding(2) var<storage, read> levelDescriptors : array<SparseVoxelOccupancyMipLevel>;
@group(0) @binding(3) var<storage, read> mipWords : array<u32>;
@group(0) @binding(4) var<storage, read> rays : array<SparseVoxelOccupancyDDARay>;
@group(0) @binding(5) var<storage, read_write> results : array<SparseVoxelOccupancyDDAResult>;
@group(0) @binding(6) var occupancyTextureRead : texture_3d<u32>;
@group(0) @binding(7) var<uniform> dispatch : SparseVoxelOccupancyDDADispatch;

const DDA_STATUS_INVALID = 0u;
const DDA_STATUS_MISS = 1u;
const DDA_STATUS_HIT = 2u;
const DDA_STATUS_QUERY_LIMIT = 3u;
const DDA_INFINITY = 3.402823466e+38f;

fn ddaNextFloatUp(value : f32) -> f32 {
    if (value == DDA_INFINITY) {
        return value;
    }
    if (value == 0.0) {
        return bitcast<f32>(0x00000001u);
    }
    let bits = bitcast<u32>(value);
    return bitcast<f32>(select(bits - 1u, bits + 1u, value > 0.0));
}

fn ddaFloorDivide(value : i32, divisor : i32) -> i32 {
    let quotient = value / divisor;
    let remainder = value % divisor;
    return quotient - select(0, 1, remainder < 0);
}

fn ddaIntersectVolume(ray : SparseVoxelOccupancyDDARay)
    -> SparseVoxelOccupancyDDAInterval {
    var interval = SparseVoxelOccupancyDDAInterval(
        0.0, ray.originMaximumDistance.w, 1u);
    if (interval.exit < 0.0 ||
        all(ray.direction.xyz == vec3<f32>(0.0))) {
        interval.valid = 0u;
        return interval;
    }
    for (var axis = 0u; axis < 3u; axis += 1u) {
        let cellSize = occupancyState.cellSize[axis];
        let minimum = f32(occupancyState.originCell[axis]) * cellSize;
        let maximum =
            (f32(occupancyState.originCell[axis]) +
             f32(occupancyState.logicalResolution[axis])) * cellSize;
        let origin = ray.originMaximumDistance[axis];
        let direction = ray.direction[axis];
        if (direction == 0.0) {
            if (origin < minimum || origin >= maximum) {
                interval.valid = 0u;
                return interval;
            }
            continue;
        }
        let firstUnsorted = (minimum - origin) / direction;
        let lastUnsorted = (maximum - origin) / direction;
        let first = min(firstUnsorted, lastUnsorted);
        let last = max(firstUnsorted, lastUnsorted);
        interval.entry = max(interval.entry, first);
        interval.exit = min(interval.exit, last);
        if (interval.exit < interval.entry) {
            interval.valid = 0u;
            return interval;
        }
    }
    return interval;
}

fn ddaBaseCellAt(
    ray : SparseVoxelOccupancyDDARay,
    distance : f32) -> SparseVoxelOccupancyDDACell {
    var result = SparseVoxelOccupancyDDACell(vec3<i32>(0), 1u);
    for (var axis = 0u; axis < 3u; axis += 1u) {
        let position =
            ray.originMaximumDistance[axis] +
            ray.direction[axis] * distance;
        let cellCoordinate =
            position / occupancyState.cellSize[axis];
        var cell = i32(floor(cellCoordinate));
        if (ray.direction[axis] < 0.0) {
            // A ray on a boundary owns the cell on the negative side. Do not
            // express this bias as a subnormal world-space offset: GPUs may
            // flush that offset to zero (notably at world coordinate zero),
            // leaving the hierarchical DDA in the same cell forever.
            let nearestBoundary = round(cellCoordinate);
            let boundaryTolerance =
                4.0 * 1.1920928955078125e-7 *
                max(1.0, abs(cellCoordinate));
            if (abs(cellCoordinate - nearestBoundary) <=
                boundaryTolerance) {
                cell = i32(nearestBoundary) - 1;
            }
        }
        result.cell[axis] = cell;
        let logical =
            result.cell[axis] - occupancyState.originCell[axis];
        if (logical < 0 ||
            logical >= i32(occupancyState.logicalResolution[axis])) {
            result.valid = 0u;
        }
    }
    return result;
}

fn ddaBaseOccupied(globalCell : vec3<i32>) -> bool {
    let logical = globalCell - occupancyState.originCell;
    if (occupancyState.valid == 0u ||
        any(logical < vec3<i32>(0)) ||
        any(logical >= vec3<i32>(occupancyState.logicalResolution))) {
        return false;
    }
    let address = sparseVoxelOccupancyAddress(
        vec3<u32>(logical), occupancyState);
    let wordCoord = sparseVoxelOccupancyWordCoord(
        address.wordIndex, occupancyState);
    return (textureLoad(
        occupancyTextureRead, vec3<i32>(wordCoord), 0).r &
        address.bitMask) != 0u;
}

fn ddaMipContains(
    level : u32,
    globalCell : vec3<i32>) -> bool {
    if (level == 0u) {
        let logical = globalCell - occupancyState.originCell;
        return all(logical >= vec3<i32>(0)) &&
            all(logical < vec3<i32>(occupancyState.logicalResolution));
    }
    let descriptor = levelDescriptors[level - 1u];
    let logical = globalCell - descriptor.originCell;
    return all(logical >= vec3<i32>(0)) &&
        all(logical < vec3<i32>(descriptor.activeResolution));
}

fn ddaMipOccupied(level : u32, globalCell : vec3<i32>) -> bool {
    if (level == 0u) {
        return ddaBaseOccupied(globalCell);
    }
    let descriptor = levelDescriptors[level - 1u];
    let logical = globalCell - descriptor.originCell;
    let address = sparseVoxelOccupancyMipAddress(
        vec3<u32>(logical), descriptor);
    return (mipWords[descriptor.wordOffset + address.wordIndex] &
        address.bitMask) != 0u;
}

fn ddaMipCellForBaseCell(
    baseCell : vec3<i32>,
    scale : u32) -> vec3<i32> {
    let signedScale = i32(scale);
    return vec3<i32>(
        ddaFloorDivide(baseCell.x, signedScale),
        ddaFloorDivide(baseCell.y, signedScale),
        ddaFloorDivide(baseCell.z, signedScale));
}

fn ddaCellExit(
    ray : SparseVoxelOccupancyDDARay,
    globalMipCell : vec3<i32>,
    scale : u32) -> f32 {
    var exit = DDA_INFINITY;
    for (var axis = 0u; axis < 3u; axis += 1u) {
        let direction = ray.direction[axis];
        if (direction == 0.0) {
            continue;
        }
        let boundaryCell = select(
            f32(globalMipCell[axis]) * f32(scale),
            (f32(globalMipCell[axis]) + 1.0) * f32(scale),
            direction > 0.0);
        let worldBoundary =
            boundaryCell * occupancyState.cellSize[axis];
        exit = min(
            exit,
            (worldBoundary - ray.originMaximumDistance[axis]) /
                direction);
    }
    return exit;
}

fn ddaAdvance(current : f32, cellExit : f32) -> f32 {
    return ddaNextFloatUp(max(current, cellExit));
}

@compute @workgroup_size(64, 1, 1)
fn traceOccupancyDDA(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let rayIndex = globalId.x;
    if (rayIndex >= dispatch.rayCount) {
        return;
    }
    let ray = rays[rayIndex];
    var output = SparseVoxelOccupancyDDAResult(
        vec3<i32>(0), DDA_STATUS_INVALID,
        0.0, 0.0, 0u, 0u, 0u,
        occupancyState.contentGeneration, 0u, 0u);
    if (occupancyState.valid == 0u) {
        results[rayIndex] = output;
        return;
    }
    let interval = ddaIntersectVolume(ray);
    if (interval.valid == 0u) {
        output.status = select(
            DDA_STATUS_MISS,
            DDA_STATUS_INVALID,
            all(ray.direction.xyz == vec3<f32>(0.0)) ||
            ray.originMaximumDistance.w < 0.0);
        results[rayIndex] = output;
        return;
    }

    let hierarchyValid =
        pyramidState.valid != 0u &&
        pyramidState.contentGeneration ==
            occupancyState.contentGeneration &&
        pyramidState.levelCount <= dispatch.levelCapacity;
    let levelCount = select(0u, pyramidState.levelCount, hierarchyValid);
    var current = interval.entry;
    while (current <= interval.exit) {
        let baseCellResult = ddaBaseCellAt(ray, current);
        if (baseCellResult.valid == 0u) {
            break;
        }
        var advanced = false;
        var descending = levelCount + 1u;
        while (descending > 0u) {
            descending -= 1u;
            let level = descending;
            let scale = select(
                levelDescriptors[max(level, 1u) - 1u].scale,
                1u,
                level == 0u);
            let mipCell =
                ddaMipCellForBaseCell(baseCellResult.cell, scale);
            if (!ddaMipContains(level, mipCell)) {
                continue;
            }
            if (output.queryCount >= dispatch.maximumQueries) {
                output.status = DDA_STATUS_QUERY_LIMIT;
                output.distance = current;
                results[rayIndex] = output;
                return;
            }
            output.queryCount += 1u;
            output.highestLevel = max(output.highestLevel, level);
            if (level == 0u) {
                output.baseCellSteps += 1u;
            }
            if (ddaMipOccupied(level, mipCell)) {
                if (level == 0u) {
                    output.status = DDA_STATUS_HIT;
                    output.globalCell = baseCellResult.cell;
                    output.distance = current;
                    results[rayIndex] = output;
                    return;
                }
                continue;
            }
            let next = ddaAdvance(
                current, ddaCellExit(ray, mipCell, scale));
            if (level > 0u) {
                output.skippedDistance += next - current;
            }
            current = next;
            advanced = true;
            break;
        }
        if (!advanced) {
            break;
        }
    }
    output.status = DDA_STATUS_MISS;
    output.distance = interval.exit;
    results[rayIndex] = output;
}
