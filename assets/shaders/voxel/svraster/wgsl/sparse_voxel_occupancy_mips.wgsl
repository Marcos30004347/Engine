#include "sparse_voxel_occupancy_common.wgsl"

@group(0) @binding(0) var<uniform> mipBuild : SparseVoxelOccupancyMipBuild;
@group(0) @binding(1) var<storage, read> dirtyWords : array<SparseVoxelOccupancyDirtyWord>;
@group(0) @binding(2) var<storage, read_write> baseAtomicWords : array<atomic<u32>>;
@group(0) @binding(3) var<storage, read_write> mipWords : array<u32>;

fn baseOccupiedAtGlobalCell(globalCell : vec3<i32>) -> bool {
    let logical = globalCell - mipBuild.baseState.originCell;
    if (any(logical < vec3<i32>(0)) ||
        any(logical >= vec3<i32>(mipBuild.baseState.logicalResolution))) {
        return false;
    }
    let address = sparseVoxelOccupancyAddress(
        vec3<u32>(logical),
        mipBuild.baseState);
    return (atomicLoad(&baseAtomicWords[address.wordIndex]) &
        address.bitMask) != 0u;
}

fn mipOccupiedAtGlobalCell(globalCell : vec3<i32>) -> bool {
    if (!sparseVoxelOccupancyMipContainsGlobalCell(
            globalCell, mipBuild.childLevel)) {
        return false;
    }
    let logical = globalCell - mipBuild.childLevel.originCell;
    let address = sparseVoxelOccupancyMipAddress(
        vec3<u32>(logical),
        mipBuild.childLevel);
    return (mipWords[mipBuild.childLevel.wordOffset + address.wordIndex] &
        address.bitMask) != 0u;
}

fn rebuildDirtyParentWord(dirtyIndex : u32, childIsBase : bool) {
    let dirty = dirtyWords[dirtyIndex];
    if (dirty.wordIndex >= mipBuild.parentLevel.wordCount) {
        return;
    }
    let wordCoord = sparseVoxelOccupancyMipWordCoord(
        dirty.wordIndex,
        mipBuild.parentLevel);
    var recomputedBits = 0u;
    for (var bit = 0u; bit < 32u; bit += 1u) {
        let bitMask = 1u << bit;
        if ((dirty.bitMask & bitMask) == 0u) {
            continue;
        }
        let physicalX = wordCoord.x * 32u + bit;
        if (physicalX >= mipBuild.parentLevel.logicalResolution.x) {
            continue;
        }
        let logical = sparseVoxelOccupancyMipLogicalCellFromPhysical(
            vec3<u32>(physicalX, wordCoord.y, wordCoord.z),
            mipBuild.parentLevel);
        if (any(logical >= mipBuild.parentLevel.activeResolution)) {
            continue;
        }
        let parentGlobal =
            mipBuild.parentLevel.originCell + vec3<i32>(logical);
        var occupied = false;
        for (var childIndex = 0u; childIndex < 8u; childIndex += 1u) {
            let childGlobal = parentGlobal * 2 +
                vec3<i32>(
                    i32(childIndex & 1u),
                    i32((childIndex >> 1u) & 1u),
                    i32((childIndex >> 2u) & 1u));
            if (childIsBase) {
                occupied = occupied ||
                    baseOccupiedAtGlobalCell(childGlobal);
            } else {
                occupied = occupied ||
                    mipOccupiedAtGlobalCell(childGlobal);
            }
        }
        if (occupied) {
            recomputedBits |= bitMask;
        }
    }
    let destination =
        mipBuild.parentLevel.wordOffset + dirty.wordIndex;
    let oldWord = mipWords[destination];
    mipWords[destination] =
        (oldWord & ~dirty.bitMask) |
        (recomputedBits & dirty.bitMask);
}

fn rebuildFullParentWord(wordIndex : u32, childIsBase : bool) {
    if (wordIndex >= mipBuild.parentLevel.wordCount) {
        return;
    }
    let wordCoord = sparseVoxelOccupancyMipWordCoord(
        wordIndex,
        mipBuild.parentLevel);
    var recomputedBits = 0u;
    for (var bit = 0u; bit < 32u; bit += 1u) {
        let physicalX = wordCoord.x * 32u + bit;
        if (physicalX >= mipBuild.parentLevel.logicalResolution.x) {
            continue;
        }
        let logical = sparseVoxelOccupancyMipLogicalCellFromPhysical(
            vec3<u32>(physicalX, wordCoord.y, wordCoord.z),
            mipBuild.parentLevel);
        if (any(logical >= mipBuild.parentLevel.activeResolution)) {
            continue;
        }
        let parentGlobal =
            mipBuild.parentLevel.originCell + vec3<i32>(logical);
        var occupied = false;
        for (var childIndex = 0u; childIndex < 8u; childIndex += 1u) {
            let childGlobal = parentGlobal * 2 +
                vec3<i32>(
                    i32(childIndex & 1u),
                    i32((childIndex >> 1u) & 1u),
                    i32((childIndex >> 2u) & 1u));
            if (childIsBase) {
                occupied = occupied ||
                    baseOccupiedAtGlobalCell(childGlobal);
            } else {
                occupied = occupied ||
                    mipOccupiedAtGlobalCell(childGlobal);
            }
        }
        if (occupied) {
            recomputedBits |= 1u << bit;
        }
    }
    mipWords[mipBuild.parentLevel.wordOffset + wordIndex] =
        recomputedBits;
}

@compute @workgroup_size(64, 1, 1)
fn buildMipFromBase(@builtin(global_invocation_id) globalId : vec3<u32>) {
    if (globalId.x >= mipBuild.dirtyWordCount) {
        return;
    }
    rebuildDirtyParentWord(globalId.x, true);
}

@compute @workgroup_size(64, 1, 1)
fn buildMipFromMip(@builtin(global_invocation_id) globalId : vec3<u32>) {
    if (globalId.x >= mipBuild.dirtyWordCount) {
        return;
    }
    rebuildDirtyParentWord(globalId.x, false);
}

@compute @workgroup_size(64, 1, 1)
fn buildFullMipFromBase(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    if (globalId.x >= mipBuild.dirtyWordCount) {
        return;
    }
    rebuildFullParentWord(globalId.x, true);
}

@compute @workgroup_size(64, 1, 1)
fn buildFullMipFromMip(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    if (globalId.x >= mipBuild.dirtyWordCount) {
        return;
    }
    rebuildFullParentWord(globalId.x, false);
}
