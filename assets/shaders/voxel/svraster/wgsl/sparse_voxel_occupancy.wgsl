#include "sparse_voxel_occupancy_common.wgsl"

@group(0) @binding(0) var<uniform> occupancyState : SparseVoxelOccupancyVolume;
@group(0) @binding(2) var<storage, read_write> occupancyAtomicWords : array<atomic<u32>>;
@group(0) @binding(3) var<storage, read> dirtyWords : array<SparseVoxelOccupancyDirtyWord>;
@group(0) @binding(5) var occupancyTextureWrite : texture_storage_3d<r32uint, write>;
@group(0) @binding(6) var<uniform> occupancyDispatch : SparseVoxelOccupancyDispatch;
@group(0) @binding(7) var<storage, read> sourceAABBs : array<SparseVoxelOccupancyAABB>;
@group(0) @binding(8) var<storage, read_write> denseDirtyMasks : array<u32>;
@group(0) @binding(9) var<storage, read> dirtyQueryBoxes : array<SparseVoxelOccupancyAABB>;

@compute @workgroup_size(64, 1, 1)
fn clearDirtyWords(@builtin(global_invocation_id) globalId : vec3<u32>) {
    if (globalId.x >= occupancyState.dirtyWordCount) {
        return;
    }
    let dirty = dirtyWords[globalId.x];
    atomicAnd(&occupancyAtomicWords[dirty.wordIndex], ~dirty.bitMask);
}

fn conservativeFirstOccupancyCell(
    value : f32, cellSize : f32) -> f32 {
    let scaled = value / cellSize;
    let base = floor(scaled);
    // Exact boundaries own both adjacent cells. Express this in cell
    // coordinates instead of relying on subnormals, which some GPUs flush.
    return select(base, base - 1.0, scaled == base);
}

fn conservativeLastOccupancyCell(
    value : f32, cellSize : f32) -> f32 {
    return floor(value / cellSize);
}

fn sparseVoxelOccupancyDirtyAddress(
    address : SparseVoxelOccupancyAddress) -> bool {
    var first = 0u;
    var end = occupancyState.dirtyWordCount;
    while (first < end) {
        let middle = first + (end - first) / 2u;
        let dirty = dirtyWords[middle];
        if (dirty.wordIndex < address.wordIndex) {
            first = middle + 1u;
        } else if (dirty.wordIndex > address.wordIndex) {
            end = middle;
        } else {
            return (dirty.bitMask & address.bitMask) != 0u;
        }
    }
    return false;
}

@compute @workgroup_size(64, 1, 1)
fn splatOccupancyAABBs(@builtin(global_invocation_id) globalId : vec3<u32>) {
    if (globalId.x >= occupancyDispatch.sourceAABBCount) {
        return;
    }
    let bounds = sourceAABBs[globalId.x];
    let firstCellFloat = vec3<f32>(
        conservativeFirstOccupancyCell(
            bounds.minimum.x, occupancyState.cellSize.x),
        conservativeFirstOccupancyCell(
            bounds.minimum.y, occupancyState.cellSize.y),
        conservativeFirstOccupancyCell(
            bounds.minimum.z, occupancyState.cellSize.z));
    let lastCellFloat = vec3<f32>(
        conservativeLastOccupancyCell(
            bounds.maximum.x, occupancyState.cellSize.x),
        conservativeLastOccupancyCell(
            bounds.maximum.y, occupancyState.cellSize.y),
        conservativeLastOccupancyCell(
            bounds.maximum.z, occupancyState.cellSize.z));
    let volumeFirstFloat = vec3<f32>(occupancyState.originCell);
    let volumeLast = occupancyState.originCell +
        vec3<i32>(occupancyState.logicalResolution) - vec3<i32>(1);
    let volumeLastFloat = vec3<f32>(volumeLast);
    if (any(firstCellFloat > volumeLastFloat) ||
        any(lastCellFloat < volumeFirstFloat)) {
        return;
    }
    let firstCell = vec3<i32>(max(firstCellFloat, volumeFirstFloat));
    let lastCell = vec3<i32>(min(lastCellFloat, volumeLastFloat));

    var z = firstCell.z;
    loop {
        var y = firstCell.y;
        loop {
            var x = firstCell.x;
            loop {
                let logical = vec3<u32>(
                    vec3<i32>(x, y, z) - occupancyState.originCell);
                let address = sparseVoxelOccupancyAddress(
                    logical, occupancyState);
                if (sparseVoxelOccupancyDirtyAddress(address)) {
                    atomicOr(
                        &occupancyAtomicWords[address.wordIndex],
                        address.bitMask);
                }
                if (x == lastCell.x) {
                    break;
                }
                x += 1;
            }
            if (y == lastCell.y) {
                break;
            }
            y += 1;
        }
        if (z == lastCell.z) {
            break;
        }
        z += 1;
    }
}

@compute @workgroup_size(64, 1, 1)
fn copyDirtyOccupancyWordsToTexture(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    if (globalId.x >= occupancyState.dirtyWordCount) {
        return;
    }
    let dirty = dirtyWords[globalId.x];
    let wordCoord = sparseVoxelOccupancyWordCoord(
        dirty.wordIndex,
        occupancyState);
    textureStore(
        occupancyTextureWrite,
        vec3<i32>(wordCoord),
        vec4<u32>(
            atomicLoad(&occupancyAtomicWords[dirty.wordIndex]),
            0u,
            0u,
            0u));
}

fn occupancyCellIntersectsDirtyQuery(globalCell : vec3<i32>) -> bool {
    let cellMinimum =
        vec3<f32>(globalCell) * occupancyState.cellSize;
    let cellMaximum =
        vec3<f32>(globalCell + vec3<i32>(1)) *
        occupancyState.cellSize;
    for (var boxIndex = 0u;
         boxIndex < occupancyDispatch.reserved0;
         boxIndex += 1u) {
        let box = dirtyQueryBoxes[boxIndex];
        if (all(cellMaximum > box.minimum.xyz) &&
            all(cellMinimum < box.maximum.xyz)) {
            return true;
        }
    }
    return false;
}

fn occupancyBitRangeMask(first : u32, end : u32) -> u32 {
    if (first >= end || first >= 32u) {
        return 0u;
    }
    let clampedEnd = min(end, 32u);
    var belowEnd = 0xffffffffu;
    if (clampedEnd < 32u) {
        belowEnd = (1u << clampedEnd) - 1u;
    }
    var belowFirst = 0u;
    if (first != 0u) {
        belowFirst = (1u << first) - 1u;
    }
    return belowEnd & ~belowFirst;
}

fn occupancyPhysicalIntervalWordMask(
    intervalFirst : u32,
    intervalEnd : u32,
    wordFirst : u32,
    wordEnd : u32) -> u32 {
    let first = max(intervalFirst, wordFirst);
    let end = min(intervalEnd, wordEnd);
    if (first >= end) {
        return 0u;
    }
    return occupancyBitRangeMask(
        first - wordFirst, end - wordFirst);
}

fn occupancyQueryWordMask(
    box : SparseVoxelOccupancyAABB,
    wordCoord : vec3<u32>) -> u32 {
    let physicalYZ = vec3<u32>(0u, wordCoord.y, wordCoord.z);
    let logicalYZ = sparseVoxelOccupancyLogicalCellFromPhysical(
        physicalYZ, occupancyState);
    let globalY = occupancyState.originCell.y + i32(logicalYZ.y);
    let globalZ = occupancyState.originCell.z + i32(logicalYZ.z);
    let cellMinimumYZ = vec2<f32>(
        f32(globalY) * occupancyState.cellSize.y,
        f32(globalZ) * occupancyState.cellSize.z);
    let cellMaximumYZ = vec2<f32>(
        f32(globalY + 1) * occupancyState.cellSize.y,
        f32(globalZ + 1) * occupancyState.cellSize.z);
    if (cellMaximumYZ.x <= box.minimum.y ||
        cellMinimumYZ.x >= box.maximum.y ||
        cellMaximumYZ.y <= box.minimum.z ||
        cellMinimumYZ.y >= box.maximum.z) {
        return 0u;
    }

    let volumeFirst = occupancyState.originCell.x;
    let volumeEnd = volumeFirst +
        i32(occupancyState.logicalResolution.x);
    let queryFirst = clamp(
        i32(floor(box.minimum.x / occupancyState.cellSize.x)),
        volumeFirst,
        volumeEnd);
    let queryEnd = clamp(
        i32(ceil(box.maximum.x / occupancyState.cellSize.x)),
        volumeFirst,
        volumeEnd);
    if (queryFirst >= queryEnd) {
        return 0u;
    }
    let logicalFirst = u32(queryFirst - volumeFirst);
    let length = u32(queryEnd - queryFirst);
    let resolution = occupancyState.logicalResolution.x;
    let physicalFirst =
        (logicalFirst + occupancyState.wrapOffset.x) % resolution;
    let firstLength = min(length, resolution - physicalFirst);
    let wordFirst = wordCoord.x * 32u;
    let wordEnd = min(wordFirst + 32u, resolution);
    var mask = occupancyPhysicalIntervalWordMask(
        physicalFirst,
        physicalFirst + firstLength,
        wordFirst,
        wordEnd);
    if (firstLength < length) {
        mask |= occupancyPhysicalIntervalWordMask(
            0u,
            length - firstLength,
            wordFirst,
            wordEnd);
    }
    return mask;
}

@compute @workgroup_size(64, 1, 1)
fn classifyDenseDirtyMasks(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let wordIndex = globalId.x;
    if (wordIndex >= occupancyState.wordResolution.x *
        occupancyState.wordResolution.y *
        occupancyState.wordResolution.z) {
        return;
    }
    if (occupancyDispatch.reserved0 == 0u) {
        denseDirtyMasks[wordIndex] = 0u;
        return;
    }
    let wordCoord = sparseVoxelOccupancyWordCoord(
        wordIndex, occupancyState);
    let wordFirst = wordCoord.x * 32u;
    let validBitCount = min(
        32u,
        occupancyState.logicalResolution.x - wordFirst);
    if (occupancyState.fullRebuild != 0u) {
        denseDirtyMasks[wordIndex] =
            occupancyBitRangeMask(0u, validBitCount);
        return;
    }
    var dirtyMask = 0u;
    for (var boxIndex = 0u;
         boxIndex < min(occupancyDispatch.reserved0, 3u);
         boxIndex += 1u) {
        dirtyMask |= occupancyQueryWordMask(
            dirtyQueryBoxes[boxIndex], wordCoord);
    }
    denseDirtyMasks[wordIndex] = dirtyMask;
}

@compute @workgroup_size(64, 1, 1)
fn clearDenseDirtyWords(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let wordIndex = globalId.x;
    if (wordIndex >= occupancyState.wordResolution.x *
        occupancyState.wordResolution.y *
        occupancyState.wordResolution.z) {
        return;
    }
    let dirtyMask = denseDirtyMasks[wordIndex];
    if (dirtyMask != 0u) {
        atomicAnd(
            &occupancyAtomicWords[wordIndex],
            ~dirtyMask);
    }
}

@compute @workgroup_size(64, 1, 1)
fn copyDenseDirtyWordsToTexture(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let wordIndex = globalId.x;
    if (wordIndex >= occupancyState.wordResolution.x *
        occupancyState.wordResolution.y *
        occupancyState.wordResolution.z ||
        denseDirtyMasks[wordIndex] == 0u) {
        return;
    }
    let wordCoord = sparseVoxelOccupancyWordCoord(
        wordIndex, occupancyState);
    textureStore(
        occupancyTextureWrite,
        vec3<i32>(wordCoord),
        vec4<u32>(
            atomicLoad(&occupancyAtomicWords[wordIndex]),
            0u,
            0u,
            0u));
}
