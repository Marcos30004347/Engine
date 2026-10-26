#include "sparse_voxel_types.wgsl"
#include "sparse_voxel_occupancy_common.wgsl"

struct SparseVoxelOccupancyDirtyDispatch {
    queryIndex : u32,
    queryBoxCount : u32,
    wordCapacity : u32,
    instanceCount : u32,
    cameraQueryBoxCount : u32,
    mipLevelCount : u32,
    reserved1 : u32,
    reserved2 : u32,
}

@group(0) @binding(0) var<uniform> occupancyState : SparseVoxelOccupancyVolume;
@group(0) @binding(1) var<uniform> dirtyDispatch : SparseVoxelOccupancyDirtyDispatch;
@group(0) @binding(2) var<storage, read_write> dirtyQueryBoxes : array<SparseVoxelOccupancyAABB>;
@group(0) @binding(3) var<storage, read_write> denseDirtyMasks : array<atomic<u32>>;
@group(0) @binding(4) var<storage, read_write> dirtyWordIndices : array<u32>;
@group(0) @binding(5) var<storage, read_write> dirtyWordCount : atomic<u32>;
@group(0) @binding(6) var<storage, read_write> dirtyDispatchArgs : array<IndirectDispatchArgs>;
@group(0) @binding(7) var<storage, read_write> occupancyAtomicWords : array<atomic<u32>>;
@group(0) @binding(8) var occupancyTextureWrite : texture_storage_3d<r32uint, write>;
@group(0) @binding(9) var<storage, read> instances : array<SparseVoxelInstance>;
@group(0) @binding(10) var<storage, read> mipWordCounts : array<u32>;

var<workgroup> sparseDirtyCameraQueryActive : atomic<u32>;
var<workgroup> sparseDirtyCombinedCameraQueryActive :
    array<atomic<u32>, 3>;

fn dirtyValidWordMask(wordIndex : u32) -> u32 {
    let wordX = wordIndex % occupancyState.wordResolution.x;
    if (wordX + 1u < occupancyState.wordResolution.x ||
        (occupancyState.logicalResolution.x & 31u) == 0u) {
        return 0xffffffffu;
    }
    return (1u << (occupancyState.logicalResolution.x & 31u)) - 1u;
}

fn appendDirtyWord(wordIndex : u32, bitMask : u32) {
    let oldMask = atomicOr(&denseDirtyMasks[wordIndex], bitMask);
    if (oldMask == 0u) {
        let outputIndex = atomicAdd(&dirtyWordCount, 1u);
        if (outputIndex < dirtyDispatch.wordCapacity) {
            dirtyWordIndices[outputIndex] = wordIndex;
        }
    }
}

fn sparseDirtyBitRangeMask(first : u32, end : u32) -> u32 {
    var belowEnd = 0xffffffffu;
    if (end < 32u) {
        belowEnd = (1u << end) - 1u;
    }
    let belowFirst = select(
        0u, (1u << first) - 1u, first != 0u);
    return belowEnd & ~belowFirst;
}

fn appendDirtyPhysicalXRange(
    physicalStart : u32,
    length : u32,
    physicalY : u32,
    physicalZ : u32) {
    var physicalX = physicalStart;
    var remaining = length;
    loop {
        if (remaining == 0u) {
            break;
        }
        let wordX = physicalX >> 5u;
        let firstBit = physicalX & 31u;
        let run = min(
            remaining,
            min(
                32u - firstBit,
                occupancyState.logicalResolution.x - physicalX));
        let wordIndex = wordX +
            occupancyState.wordResolution.x *
                (physicalY +
                 occupancyState.logicalResolution.y * physicalZ);
        appendDirtyWord(
            wordIndex,
            sparseDirtyBitRangeMask(firstBit, firstBit + run));
        remaining -= run;
        physicalX =
            (physicalX + run) %
                occupancyState.logicalResolution.x;
    }
}

fn sparseDirtyQueryDispatchArgs(queryIndex : u32) -> IndirectDispatchArgs {
    if (occupancyState.fullRebuild != 0u ||
        queryIndex >= dirtyDispatch.queryBoxCount) {
        return IndirectDispatchArgs(0u, 1u, 1u, 0u);
    }
    let box = dirtyQueryBoxes[queryIndex];
    let volumeFirst = occupancyState.originCell;
    let volumeEnd = volumeFirst +
        vec3<i32>(occupancyState.logicalResolution);
    let queryFirst = clamp(
        vec3<i32>(floor(box.minimum.xyz / occupancyState.cellSize)),
        volumeFirst,
        volumeEnd);
    let queryEnd = clamp(
        vec3<i32>(ceil(box.maximum.xyz / occupancyState.cellSize)),
        volumeFirst,
        volumeEnd);
    let cellCount = vec3<u32>(max(
        queryEnd - queryFirst, vec3<i32>(0)));
    let xChunks = (cellCount.x + 31u) / 32u;
    return IndirectDispatchArgs(
        (xChunks + 3u) / 4u,
        (cellCount.y + 3u) / 4u,
        (cellCount.z + 3u) / 4u,
        0u);
}

fn sparseDirtyCameraQueryAxis(queryIndex : u32) -> u32 {
    var queryOrdinal = 0u;
    for (var axis = 0u; axis < 3u; axis += 1u) {
        if (occupancyState.scrollDelta[axis] == 0) {
            continue;
        }
        if (queryOrdinal == queryIndex) {
            return axis;
        }
        queryOrdinal += 1u;
    }
    return 3u;
}

fn sparseDirtyIntersectsBox(
    minimum : vec3<f32>,
    maximum : vec3<f32>,
    box : SparseVoxelOccupancyAABB) -> bool {
    return all(maximum >= box.minimum.xyz) &&
        all(minimum <= box.maximum.xyz);
}

fn sparseDirtyResetState() {
    atomicStore(&dirtyWordCount, 0u);
    dirtyDispatchArgs[0u] = IndirectDispatchArgs(0u, 1u, 1u, 0u);
    for (var queryIndex = 0u; queryIndex < 3u; queryIndex += 1u) {
        dirtyDispatchArgs[queryIndex + 1u] =
            sparseDirtyQueryDispatchArgs(queryIndex);
    }
    var fullGroupsX = 0u;
    var fullGroupsY = 1u;
    if (occupancyState.fullRebuild != 0u) {
        let fullGroupCount =
            (dirtyDispatch.wordCapacity + 63u) / 64u;
        fullGroupsX = min(fullGroupCount, 1024u);
        fullGroupsY =
            (fullGroupCount + fullGroupsX - 1u) / fullGroupsX;
    }
    dirtyDispatchArgs[4u] =
        IndirectDispatchArgs(fullGroupsX, fullGroupsY, 1u, 0u);
}

@compute @workgroup_size(1, 1, 1)
fn resetSparseDirtyState() {
    sparseDirtyResetState();
}

fn sparseDirtyCameraQueryIntersectsScene(
    queryIndex : u32, laneIndex : u32) -> bool {
    let shouldCull =
        occupancyState.fullRebuild == 0u &&
        queryIndex < dirtyDispatch.cameraQueryBoxCount &&
        queryIndex < dirtyDispatch.queryBoxCount;
    if (!shouldCull) { return true; }
    let axis = sparseDirtyCameraQueryAxis(queryIndex);
    if (axis >= 3u) { return true; }
    let incoming = dirtyQueryBoxes[queryIndex];
    var outgoing = incoming;
    let worldExtent =
        occupancyState.cellSize[axis] *
        f32(occupancyState.logicalResolution[axis]);
    let outgoingShift = select(
        worldExtent,
        -worldExtent,
        occupancyState.scrollDelta[axis] > 0);
    outgoing.minimum[axis] += outgoingShift;
    outgoing.maximum[axis] += outgoingShift;
    // The physical cells covered by this slab came from the old volume. On a
    // diagonal scroll, their ranges on the other axes follow the old origin.
    for (var otherAxis = 0u; otherAxis < 3u; otherAxis += 1u) {
        if (otherAxis == axis) { continue; }
        let oldOriginShift =
            -f32(occupancyState.scrollDelta[otherAxis]) *
            occupancyState.cellSize[otherAxis];
        outgoing.minimum[otherAxis] += oldOriginShift;
        outgoing.maximum[otherAxis] += oldOriginShift;
    }
    for (var instanceIndex = laneIndex;
         instanceIndex < dirtyDispatch.instanceCount;
         instanceIndex += 64u) {
        let instance = instances[instanceIndex];
        if (sparseDirtyIntersectsBox(
                instance.worldBoundsMin.xyz,
                instance.worldBoundsMax.xyz,
                incoming) ||
            sparseDirtyIntersectsBox(
                instance.worldBoundsMin.xyz,
                instance.worldBoundsMax.xyz,
                outgoing)) {
            return true;
        }
    }
    return false;
}

@compute @workgroup_size(64, 1, 1)
fn cullSparseDirtyCameraQueries(
    @builtin(workgroup_id) workgroupId : vec3<u32>,
    @builtin(local_invocation_index) localIndex : u32) {
    let queryIndex = workgroupId.x;
    if (localIndex == 0u) {
        atomicStore(&sparseDirtyCameraQueryActive, 0u);
    }
    workgroupBarrier();

    let shouldCull =
        occupancyState.fullRebuild == 0u &&
        queryIndex < dirtyDispatch.cameraQueryBoxCount &&
        queryIndex < dirtyDispatch.queryBoxCount;
    if (shouldCull &&
        sparseDirtyCameraQueryIntersectsScene(queryIndex, localIndex)) {
        atomicStore(&sparseDirtyCameraQueryActive, 1u);
    }
    workgroupBarrier();

    if (localIndex == 0u && shouldCull &&
        atomicLoad(&sparseDirtyCameraQueryActive) == 0u) {
        dirtyDispatchArgs[queryIndex + 1u] =
            IndirectDispatchArgs(0u, 1u, 1u, 0u);
        dirtyQueryBoxes[queryIndex] =
            SparseVoxelOccupancyAABB(
                vec4<f32>(3.402823466e38, 0.0, 0.0, 0.0),
                vec4<f32>(-3.402823466e38, 0.0, 0.0, 0.0));
    }
}

// Resetting the five indirect argument records and testing three camera slabs
// are both tiny operations. Keeping them as separate passes introduced another
// encoder boundary/timestamp attribution point, which could make this
// 192-thread operation appear to own tens of milliseconds. One 192-thread
// workgroup provides the required global ordering and performs both jobs.
@compute @workgroup_size(192, 1, 1)
fn resetAndCullSparseDirtyCameraQueries(
    @builtin(local_invocation_index) localIndex : u32) {
    if (localIndex < 3u) {
        atomicStore(
            &sparseDirtyCombinedCameraQueryActive[localIndex], 0u);
    }
    if (localIndex == 0u) {
        sparseDirtyResetState();
    }
    workgroupBarrier();

    let queryIndex = localIndex / 64u;
    let laneIndex = localIndex & 63u;
    let shouldCull =
        occupancyState.fullRebuild == 0u &&
        queryIndex < dirtyDispatch.cameraQueryBoxCount &&
        queryIndex < dirtyDispatch.queryBoxCount;
    if (shouldCull &&
        sparseDirtyCameraQueryIntersectsScene(queryIndex, laneIndex)) {
        atomicStore(
            &sparseDirtyCombinedCameraQueryActive[queryIndex], 1u);
    }
    workgroupBarrier();

    if (laneIndex == 0u && shouldCull &&
        atomicLoad(
            &sparseDirtyCombinedCameraQueryActive[queryIndex]) == 0u) {
        dirtyDispatchArgs[queryIndex + 1u] =
            IndirectDispatchArgs(0u, 1u, 1u, 0u);
        dirtyQueryBoxes[queryIndex] =
            SparseVoxelOccupancyAABB(
                vec4<f32>(3.402823466e38, 0.0, 0.0, 0.0),
                vec4<f32>(-3.402823466e38, 0.0, 0.0, 0.0));
    }
}

@compute @workgroup_size(64, 1, 1)
fn markAllDirtyWords(
    @builtin(global_invocation_id) globalId : vec3<u32>,
    @builtin(num_workgroups) numWorkgroups : vec3<u32>) {
    let wordIndex =
        globalId.x + globalId.y * numWorkgroups.x * 64u;
    if (occupancyState.fullRebuild == 0u ||
        wordIndex >= dirtyDispatch.wordCapacity) {
        return;
    }
    atomicStore(
        &denseDirtyMasks[wordIndex],
        dirtyValidWordMask(wordIndex));
}

@compute @workgroup_size(4, 4, 4)
fn markSparseDirtyQuery(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    if (occupancyState.fullRebuild != 0u ||
        dirtyDispatch.queryIndex >= dirtyDispatch.queryBoxCount) {
        return;
    }
    let box = dirtyQueryBoxes[dirtyDispatch.queryIndex];
    let volumeFirst = occupancyState.originCell;
    let volumeEnd = volumeFirst +
        vec3<i32>(occupancyState.logicalResolution);
    let queryFirst = clamp(
        vec3<i32>(floor(box.minimum.xyz / occupancyState.cellSize)),
        volumeFirst,
        volumeEnd);
    let queryEnd = clamp(
        vec3<i32>(ceil(box.maximum.xyz / occupancyState.cellSize)),
        volumeFirst,
        volumeEnd);
    if (any(queryFirst >= queryEnd)) {
        return;
    }
    let y = queryFirst.y + i32(globalId.y);
    let z = queryFirst.z + i32(globalId.z);
    let xFirst = queryFirst.x + i32(globalId.x * 32u);
    if (y >= queryEnd.y || z >= queryEnd.z || xFirst >= queryEnd.x) {
        return;
    }
    let xEnd = min(xFirst + 32, queryEnd.x);
    let logical = vec3<u32>(
        vec3<i32>(xFirst, y, z) - occupancyState.originCell);
    let physicalY =
        (logical.y + occupancyState.wrapOffset.y) %
            occupancyState.logicalResolution.y;
    let physicalZ =
        (logical.z + occupancyState.wrapOffset.z) %
            occupancyState.logicalResolution.z;
    let physicalX =
        (logical.x + occupancyState.wrapOffset.x) %
            occupancyState.logicalResolution.x;
    appendDirtyPhysicalXRange(
        physicalX,
        u32(xEnd - xFirst),
        physicalY,
        physicalZ);
}

@compute @workgroup_size(1, 1, 1)
fn finalizeSparseDirtyState() {
    let count = min(
        atomicLoad(&dirtyWordCount), dirtyDispatch.wordCapacity);
    let groupCount = (count + 63u) / 64u;
    var groupsX = 0u;
    var groupsY = 1u;
    if (groupCount != 0u) {
        groupsX = min(groupCount, 1024u);
        groupsY = (groupCount + groupsX - 1u) / groupsX;
    }
    dirtyDispatchArgs[0u] =
        IndirectDispatchArgs(groupsX, groupsY, 1u, 0u);
    let rebuildMips =
        occupancyState.fullRebuild != 0u || count != 0u;
    for (var mipIndex = 0u;
         mipIndex < dirtyDispatch.mipLevelCount;
         mipIndex += 1u) {
        let mipGroupCount = select(
            0u,
            (mipWordCounts[mipIndex] + 63u) / 64u,
            rebuildMips);
        dirtyDispatchArgs[5u + mipIndex] =
            IndirectDispatchArgs(
                mipGroupCount, 1u, 1u, 0u);
    }
}

@compute @workgroup_size(64, 1, 1)
fn clearSparseDirtyWords(
    @builtin(global_invocation_id) globalId : vec3<u32>,
    @builtin(num_workgroups) numWorkgroups : vec3<u32>) {
    if (occupancyState.fullRebuild != 0u) {
        return;
    }
    let dirtyIndex =
        globalId.x + globalId.y * numWorkgroups.x * 64u;
    let count = min(
        atomicLoad(&dirtyWordCount), dirtyDispatch.wordCapacity);
    if (dirtyIndex >= count) {
        return;
    }
    let wordIndex = dirtyWordIndices[dirtyIndex];
    let dirtyMask = atomicLoad(&denseDirtyMasks[wordIndex]);
    atomicAnd(&occupancyAtomicWords[wordIndex], ~dirtyMask);
}

@compute @workgroup_size(64, 1, 1)
fn publishSparseDirtyWords(
    @builtin(global_invocation_id) globalId : vec3<u32>,
    @builtin(num_workgroups) numWorkgroups : vec3<u32>) {
    if (occupancyState.fullRebuild != 0u) {
        return;
    }
    let dirtyIndex =
        globalId.x + globalId.y * numWorkgroups.x * 64u;
    let count = min(
        atomicLoad(&dirtyWordCount), dirtyDispatch.wordCapacity);
    if (dirtyIndex >= count) {
        return;
    }
    let wordIndex = dirtyWordIndices[dirtyIndex];
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
    atomicStore(&denseDirtyMasks[wordIndex], 0u);
}

@compute @workgroup_size(64, 1, 1)
fn clearFullDirtyWords(
    @builtin(global_invocation_id) globalId : vec3<u32>,
    @builtin(num_workgroups) numWorkgroups : vec3<u32>) {
    let wordIndex =
        globalId.x + globalId.y * numWorkgroups.x * 64u;
    if (occupancyState.fullRebuild == 0u ||
        wordIndex >= dirtyDispatch.wordCapacity) {
        return;
    }
    atomicStore(&occupancyAtomicWords[wordIndex], 0u);
}

@compute @workgroup_size(64, 1, 1)
fn publishFullDirtyWords(
    @builtin(global_invocation_id) globalId : vec3<u32>,
    @builtin(num_workgroups) numWorkgroups : vec3<u32>) {
    let wordIndex =
        globalId.x + globalId.y * numWorkgroups.x * 64u;
    if (occupancyState.fullRebuild == 0u ||
        wordIndex >= dirtyDispatch.wordCapacity) {
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
    atomicStore(&denseDirtyMasks[wordIndex], 0u);
}
