#include "gpusigneddistancefields_common.wgsl"

struct CombineDispatchUniforms {
    cascade : CascadeInfoGPU,
    cascadeIndex : u32,
    treeIteration : u32,
    cascadeCount : u32,
    dispatchAllCascades : u32,
    totalBricks : u32,
    atlasBricksPerAxis : u32,
    _padding0 : u32,
    _padding1 : u32,
}

@group(0) @binding(0) var<uniform> gUniforms : CombineDispatchUniforms;
@group(0) @binding(1) var<storage, read> CascadeInfos : array<CascadeInfoGPU>;
@group(0) @binding(2) var<storage, read_write> StaticBrickMap : array<u32>;
@group(0) @binding(3) var<storage, read_write> SkinnedBrickMap : array<u32>;
@group(0) @binding(4) var<storage, read_write> CombinedBrickMap : array<u32>;
@group(0) @binding(5) var<storage, read_write> CombinedAabbTree : array<u32>;
@group(0) @binding(6) var<storage, read_write> ContextBrickVoxelMap : array<u32>;
@group(0) @binding(7) var<storage, read_write> ContextBrickAabb : array<u32>;
@group(0) @binding(8) var<storage, read_write> ContextBrickFreeList : array<u32>;
@group(0) @binding(9) var<storage, read_write> ContextBrickDirtyList : array<u32>;
@group(0) @binding(10) var<storage, read_write> ContextBrickMergeList : array<u32>;
@group(0) @binding(11) var<storage, read_write> ContextBrickEikonalCounters : array<u32>;
@group(0) @binding(12) var<storage, read_write> ContextCounters : array<atomic<u32>>;
@group(0) @binding(13) var<storage, read_write> ContextIndirectArgs : array<u32>;
@group(0) @binding(14) var SdfAtlas : texture_storage_3d<r32float, read_write>;

var<workgroup> gsEikonalSdfCache : array<f32, GPUSignedDistanceFields_BrickCellCount>;
var<workgroup> gsTreeMin : array<atomic<u32>, 3>;
var<workgroup> gsTreeMax : array<atomic<u32>, 3>;

fn CombineCascadeIndex(groupY: u32) -> u32 {
    return select(gUniforms.cascadeIndex, groupY, gUniforms.dispatchAllCascades != 0u);
}

fn LoadCascadeInfo(cascadeIndex: u32) -> CascadeInfoGPU {
    return CascadeInfos[cascadeIndex];
}

fn VoxelBase(cascadeIndex: u32) -> u32 {
    return cascadeIndex * GPUSignedDistanceFields_CascadeVoxelCount;
}

fn TreeWordBase(cascadeIndex: u32) -> u32 {
    return cascadeIndex * GPUSignedDistanceFields_BuildTreeFloat4Count * 4u;
}

fn LoadAtlasBrixelDistance(brickId: u32, localCoord: vec3<i32>) -> f32 {
    if (any(localCoord < vec3<i32>(0, 0, 0)) || any(localCoord >= vec3<i32>(8, 8, 8))) {
        return 1.0;
    }

    return textureLoad(
        SdfAtlas,
        vec3<i32>(GPUSignedDistanceFieldsAtlasCellCoord(brickId, vec3<u32>(localCoord), gUniforms.atlasBricksPerAxis))).x;
}

fn StoreAtlasBrixelDistance(brickId: u32, localCoord: vec3<u32>, distanceValue: f32) {
    textureStore(
        SdfAtlas,
        vec3<i32>(GPUSignedDistanceFieldsAtlasCellCoord(brickId, localCoord, gUniforms.atlasBricksPerAxis)),
        vec4<f32>(clamp(distanceValue, 0.0, 1.0), 0.0, 0.0, 0.0));
}

fn EikonalMin(a: f32, b: f32) -> f32 {
    return GPUSignedDistanceFieldsUnsignedMin(a, b);
}

fn Eikonal1D(x: f32, y: f32, z: f32, d: f32) -> f32 {
    let xyz = EikonalMin(x, EikonalMin(y, z));
    return xyz + d * GPUSignedDistanceFieldsGetSign(xyz);
}

fn Eikonal2D(x: f32, y: f32, d: f32) -> f32 {
    let xy = x + y;
    let v = xy * xy - 2.0 * (x * x + y * y - d * d);
    if (v < 0.0) {
        return 1.0;
    }
    return 0.5 * (xy + sqrt(v) * GPUSignedDistanceFieldsGetSign(xy));
}

fn Eikonal3D(x: f32, y: f32, z: f32, d: f32) -> f32 {
    let xyz = x + y + z;
    let v = xyz * xyz - 3.0 * (x * x + y * y + z * z - d * d);
    if (v < 0.0) {
        return 1.0;
    }
    return (xyz + sqrt(v) * GPUSignedDistanceFieldsGetSign(xyz)) / 3.0;
}

fn LoadEikonalCache(coord: vec3<i32>) -> f32 {
    if (any(coord < vec3<i32>(0, 0, 0)) || any(coord >= vec3<i32>(8, 8, 8))) {
        return 1.0;
    }
    return gsEikonalSdfCache[GPUSignedDistanceFieldsFlatten3D(vec3<u32>(coord), GPUSignedDistanceFields_BrickResolution)];
}

fn StoreEikonalCache(coord: vec3<i32>, value: f32) {
    gsEikonalSdfCache[GPUSignedDistanceFieldsFlatten3D(vec3<u32>(coord), GPUSignedDistanceFields_BrickResolution)] = value;
}

fn StoreTreeCoord3(cascadeIndex: u32, wordOffset: u32, value: vec3<u32>) {
    let base = TreeWordBase(cascadeIndex) + wordOffset;
    CombinedAabbTree[base + 0u] = value.x;
    CombinedAabbTree[base + 1u] = value.y;
    CombinedAabbTree[base + 2u] = value.z;
}

fn LoadTreeCoord3(cascadeIndex: u32, wordOffset: u32) -> vec3<u32> {
    let base = TreeWordBase(cascadeIndex) + wordOffset;
    return vec3<u32>(
        CombinedAabbTree[base + 0u],
        CombinedAabbTree[base + 1u],
        CombinedAabbTree[base + 2u]);
}

fn TreeAabbIsEmpty(aabbMin: vec3<u32>, aabbMax: vec3<u32>) -> bool {
    return aabbMin.x == aabbMax.x &&
        aabbMin.y == aabbMax.y &&
        aabbMin.z == aabbMax.z;
}

fn StoreContextIndirectArgs(byteOffset: u32, x: u32, y: u32, z: u32) {
    let wordOffset = byteOffset / 4u;
    ContextIndirectArgs[wordOffset + 0u] = x;
    ContextIndirectArgs[wordOffset + 1u] = y;
    ContextIndirectArgs[wordOffset + 2u] = z;
    ContextIndirectArgs[wordOffset + 3u] = 0u;
}

@compute @workgroup_size(64, 1, 1)
fn CollectDirtyBricksCS(@builtin(global_invocation_id) dispatchThreadID: vec3<u32>) {
    let brickId = dispatchThreadID.x;
    if (brickId >= gUniforms.totalBricks) {
        return;
    }

    let eikonalCounter = ContextBrickEikonalCounters[brickId];
    let voxelId = ContextBrickVoxelMap[brickId];
    if (!GPUSignedDistanceFieldsIsBrickValid(voxelId)) {
        return;
    }

    if (eikonalCounter < 16u) {
        let offset = atomicAdd(&ContextCounters[GPUSignedDistanceFields_ContextCounter_DirtyBricks], 1u);
        ContextBrickDirtyList[offset] = brickId;
        ContextBrickEikonalCounters[brickId] = eikonalCounter + 16u;
    }
}

@compute @workgroup_size(1, 1, 1)
fn PrepareEikonalArgsCS(@builtin(global_invocation_id) _dispatchThreadID: vec3<u32>) {
    StoreContextIndirectArgs(
        GPUSignedDistanceFields_IndirectOffset_Eikonal,
        atomicLoad(&ContextCounters[GPUSignedDistanceFields_ContextCounter_DirtyBricks]),
        1u,
        1u);
    atomicStore(&ContextCounters[GPUSignedDistanceFields_ContextCounter_DirtyBricks], 0u);
}

@compute @workgroup_size(512, 1, 1)
fn EikonalCS(@builtin(local_invocation_id) localInvocationID: vec3<u32>, @builtin(workgroup_id) groupId: vec3<u32>) {
    let gtid = localInvocationID.x;
    let brickId = ContextBrickDirtyList[groupId.x];
    let voxelId = ContextBrickVoxelMap[brickId];
    if (!GPUSignedDistanceFieldsIsBrickValid(voxelId)) {
        return;
    }

    let localCoordU = GPUSignedDistanceFieldsUnflatten3D(gtid, GPUSignedDistanceFields_BrickResolution);
    let localCoord = vec3<i32>(localCoordU);
    let cellDistance = 1.0 / 8.0;
    var e = LoadAtlasBrixelDistance(brickId, localCoord);

    StoreEikonalCache(localCoord, e);
    workgroupBarrier();

    for (var j = 0u; j < 3u; j = j + 1u) {
        for (var i = 0u; i < 4u; i = i + 1u) {
            let d = 1u << (3u - i);
            let di = i32(d);
            var minDists = vec3<f32>(1.0, 1.0, 1.0);
            minDists.x = EikonalMin(LoadEikonalCache(localCoord + vec3<i32>(di, 0, 0)), LoadEikonalCache(localCoord + vec3<i32>(-di, 0, 0)));
            minDists.y = EikonalMin(LoadEikonalCache(localCoord + vec3<i32>(0, di, 0)), LoadEikonalCache(localCoord + vec3<i32>(0, -di, 0)));
            minDists.z = EikonalMin(LoadEikonalCache(localCoord + vec3<i32>(0, 0, di)), LoadEikonalCache(localCoord + vec3<i32>(0, 0, -di)));
            minDists = abs(minDists);

            let e10 = Eikonal1D(minDists.x, minDists.y, minDists.z, f32(d) * cellDistance);
            let e20 = Eikonal2D(minDists.x, minDists.y, f32(d) * cellDistance);
            let e21 = Eikonal2D(minDists.x, minDists.z, f32(d) * cellDistance);
            let e22 = Eikonal2D(minDists.z, minDists.y, f32(d) * cellDistance);
            let e30 = Eikonal3D(minDists.x, minDists.y, minDists.z, f32(d) * cellDistance);
            e = EikonalMin(e, EikonalMin(e10, EikonalMin(e20, EikonalMin(e21, EikonalMin(e22, e30)))));
            e = EikonalMin(e, e10);
            StoreEikonalCache(localCoord, e);
            workgroupBarrier();
        }
    }

    StoreAtlasBrixelDistance(brickId, localCoordU, e);
}

@compute @workgroup_size(64, 1, 1)
fn MergeCascadesCS(@builtin(global_invocation_id) dispatchThreadID: vec3<u32>) {
    let cascadeIndex = CombineCascadeIndex(dispatchThreadID.y);
    let cascade = LoadCascadeInfo(cascadeIndex);
    let voxelIndex = dispatchThreadID.x;
    if (voxelIndex >= GPUSignedDistanceFields_CascadeVoxelCount) {
        return;
    }

    let wrappedVoxelIndex = GPUSignedDistanceFieldsWrapFlatCoords(cascade, voxelIndex);
    let brickMapIndex = VoxelBase(cascadeIndex) + wrappedVoxelIndex;
    let brickA = StaticBrickMap[brickMapIndex];
    let brickB = SkinnedBrickMap[brickMapIndex];

    if (GPUSignedDistanceFieldsIsBrickValid(brickA) && GPUSignedDistanceFieldsIsBrickValid(brickB)) {
        let offset = atomicAdd(&ContextCounters[GPUSignedDistanceFields_ContextCounter_MergeBricks], 1u);
        ContextBrickMergeList[offset * 2u + 0u] = brickA;
        ContextBrickMergeList[offset * 2u + 1u] = brickB;
        CombinedBrickMap[brickMapIndex] = brickB;

        let brickAAabb = ContextBrickAabb[brickA];
        let brickBAabb = ContextBrickAabb[brickB];
        let aabbMin = min(GPUSignedDistanceFieldsUnpackBrickAabbMin(brickAAabb), GPUSignedDistanceFieldsUnpackBrickAabbMin(brickBAabb));
        let aabbMax = max(GPUSignedDistanceFieldsUnpackBrickAabbMax(brickAAabb), GPUSignedDistanceFieldsUnpackBrickAabbMax(brickBAabb));
        ContextBrickAabb[brickB] = GPUSignedDistanceFieldsPackBrickAabb(aabbMin, aabbMax);
    } else if (brickA == GPUSignedDistanceFields_UninitializedId && brickB == GPUSignedDistanceFields_UninitializedId) {
        CombinedBrickMap[brickMapIndex] = GPUSignedDistanceFields_UninitializedId;
    } else if (GPUSignedDistanceFieldsIsBrickValid(brickB)) {
        CombinedBrickMap[brickMapIndex] = brickB;
    } else {
        CombinedBrickMap[brickMapIndex] = brickA;
    }
}

@compute @workgroup_size(1, 1, 1)
fn PrepareMergeBricksArgsCS(@builtin(global_invocation_id) _dispatchThreadID: vec3<u32>) {
    let mergeCount = atomicLoad(&ContextCounters[GPUSignedDistanceFields_ContextCounter_MergeBricks]);
    StoreContextIndirectArgs(GPUSignedDistanceFields_IndirectOffset_MergeBricks, mergeCount * 8u, 1u, 1u);
    atomicStore(&ContextCounters[GPUSignedDistanceFields_ContextCounter_MergeBricks], 0u);
}

@compute @workgroup_size(64, 1, 1)
fn MergeBricksCS(@builtin(local_invocation_id) localInvocationID: vec3<u32>, @builtin(workgroup_id) groupId: vec3<u32>) {
    let mergeCount = ContextIndirectArgs[GPUSignedDistanceFields_IndirectOffset_MergeBricks / 4u] / 8u;
    let mergeIndex = groupId.x / 8u;
    let stampIndex = groupId.x % 8u;
    let voxelIndex = localInvocationID.x + stampIndex * 64u;
    if (mergeIndex >= mergeCount || voxelIndex >= GPUSignedDistanceFields_BrickCellCount) {
        return;
    }

    let brickA = ContextBrickMergeList[mergeIndex * 2u + 0u];
    let brickB = ContextBrickMergeList[mergeIndex * 2u + 1u];
    let localCoord = GPUSignedDistanceFieldsUnflatten3D(voxelIndex, GPUSignedDistanceFields_BrickResolution);
    let sdfA = LoadAtlasBrixelDistance(brickA, vec3<i32>(localCoord));
    let sdfB = LoadAtlasBrixelDistance(brickB, vec3<i32>(localCoord));
    StoreAtlasBrixelDistance(brickB, localCoord, min(sdfA, sdfB));
}

@compute @workgroup_size(4, 4, 4)
fn BuildTreeAabbCS(@builtin(local_invocation_id) gtid: vec3<u32>, @builtin(workgroup_id) groupId: vec3<u32>) {
    let layerIndex = gUniforms.treeIteration;
    var treeDispatchSize = 1u;
    if (layerIndex == 0u) {
        treeDispatchSize = 16u;
    } else if (layerIndex == 1u) {
        treeDispatchSize = 4u;
    }

    let cascadeIndex = select(gUniforms.cascadeIndex, groupId.x / treeDispatchSize, gUniforms.dispatchAllCascades != 0u);
    let cascade = LoadCascadeInfo(cascadeIndex);
    let localGroupId = vec3<u32>(
        select(groupId.x, groupId.x % treeDispatchSize, gUniforms.dispatchAllCascades != 0u),
        groupId.y,
        groupId.z);

    if (all(gtid == vec3<u32>(0u, 0u, 0u))) {
        atomicStore(&gsTreeMin[0], 0xffffffffu);
        atomicStore(&gsTreeMin[1], 0xffffffffu);
        atomicStore(&gsTreeMin[2], 0xffffffffu);
        atomicStore(&gsTreeMax[0], 0u);
        atomicStore(&gsTreeMax[1], 0u);
        atomicStore(&gsTreeMax[2], 0u);
    }
    workgroupBarrier();

    if (layerIndex == 0u) {
        let childCoord = gtid;
        let nodeOffset = localGroupId * 4u;
        let voxelCoord = nodeOffset + childCoord;
        let wrapped = GPUSignedDistanceFieldsWrapCoords(cascade, voxelCoord);
        let brickId = CombinedBrickMap[VoxelBase(cascadeIndex) + GPUSignedDistanceFieldsFlatten3D(wrapped, GPUSignedDistanceFields_CascadeResolution)];
        if (GPUSignedDistanceFieldsIsBrickValid(brickId)) {
            let packedAabb = ContextBrickAabb[brickId];
            let aabbMin = GPUSignedDistanceFieldsUnpackBrickAabbMin(packedAabb);
            let aabbMax = GPUSignedDistanceFieldsUnpackBrickAabbMax(packedAabb);
            _ = atomicMin(&gsTreeMin[0], childCoord.x * 8u + aabbMin.x);
            _ = atomicMin(&gsTreeMin[1], childCoord.y * 8u + aabbMin.y);
            _ = atomicMin(&gsTreeMin[2], childCoord.z * 8u + aabbMin.z);
            _ = atomicMax(&gsTreeMax[0], childCoord.x * 8u + aabbMax.x);
            _ = atomicMax(&gsTreeMax[1], childCoord.y * 8u + aabbMax.y);
            _ = atomicMax(&gsTreeMax[2], childCoord.z * 8u + aabbMax.z);
        }
        workgroupBarrier();

        if (all(gtid == vec3<u32>(0u, 0u, 0u))) {
            let flatStampIndex = TreeWordBase(cascadeIndex) + GPUSignedDistanceFieldsFlatten3D(localGroupId, 16u);
            let minPack = GPUSignedDistanceFieldsFlatten3D(vec3<u32>(
                atomicLoad(&gsTreeMin[0]),
                atomicLoad(&gsTreeMin[1]),
                atomicLoad(&gsTreeMin[2])) & vec3<u32>(0x1fu, 0x1fu, 0x1fu), 32u);
            let maxPack = GPUSignedDistanceFieldsFlatten3D(vec3<u32>(
                atomicLoad(&gsTreeMax[0]),
                atomicLoad(&gsTreeMax[1]),
                atomicLoad(&gsTreeMax[2])) & vec3<u32>(0x1fu, 0x1fu, 0x1fu), 32u);
            CombinedAabbTree[flatStampIndex] = minPack | (maxPack << 16u);
        }
        return;
    }

    if (layerIndex == 1u) {
        let childCoord = gtid * 4u + localGroupId * 16u;
        let childIndex = GPUSignedDistanceFieldsFlatten3D(childCoord / 4u, 16u);
        let bottomNode = CombinedAabbTree[TreeWordBase(cascadeIndex) + childIndex];
        if (bottomNode != GPUSignedDistanceFields_InvalidBottomAabbNode) {
            let aabbMin = GPUSignedDistanceFieldsUnflatten3D(bottomNode & 0x7fffu, 32u);
            let aabbMax = GPUSignedDistanceFieldsUnflatten3D((bottomNode >> 16u) & 0x7fffu, 32u);
            _ = atomicMin(&gsTreeMin[0], childCoord.x * 8u + aabbMin.x);
            _ = atomicMin(&gsTreeMin[1], childCoord.y * 8u + aabbMin.y);
            _ = atomicMin(&gsTreeMin[2], childCoord.z * 8u + aabbMin.z);
            _ = atomicMax(&gsTreeMax[0], childCoord.x * 8u + aabbMax.x);
            _ = atomicMax(&gsTreeMax[1], childCoord.y * 8u + aabbMax.y);
            _ = atomicMax(&gsTreeMax[2], childCoord.z * 8u + aabbMax.z);
        }
        workgroupBarrier();

        if (all(gtid == vec3<u32>(0u, 0u, 0u))) {
            let wordOffset = 16u * 16u * 16u + 6u * GPUSignedDistanceFieldsFlatten3D(localGroupId, 4u);
            if (atomicLoad(&gsTreeMin[0]) == 0xffffffffu) {
                StoreTreeCoord3(cascadeIndex, wordOffset + 0u, vec3<u32>(0u, 0u, 0u));
                StoreTreeCoord3(cascadeIndex, wordOffset + 3u, vec3<u32>(0u, 0u, 0u));
            } else {
                let localAabbMin = vec3<u32>(
                    atomicLoad(&gsTreeMin[0]),
                    atomicLoad(&gsTreeMin[1]),
                    atomicLoad(&gsTreeMin[2]));
                let localAabbMax = vec3<u32>(
                    atomicLoad(&gsTreeMax[0]) + 1u,
                    atomicLoad(&gsTreeMax[1]) + 1u,
                    atomicLoad(&gsTreeMax[2]) + 1u);
                StoreTreeCoord3(cascadeIndex, wordOffset + 0u, localAabbMin);
                StoreTreeCoord3(cascadeIndex, wordOffset + 3u, localAabbMax);
            }
        }
        return;
    }

    if (layerIndex == 2u) {
        let childIndex = GPUSignedDistanceFieldsFlatten3D(gtid, 4u);
        let wordOffset = 16u * 16u * 16u + childIndex * 6u;
        let stampAabbMin = LoadTreeCoord3(cascadeIndex, wordOffset + 0u);
        let stampAabbMax = LoadTreeCoord3(cascadeIndex, wordOffset + 3u);
        if (!TreeAabbIsEmpty(stampAabbMin, stampAabbMax)) {
            _ = atomicMin(&gsTreeMin[0], stampAabbMin.x);
            _ = atomicMin(&gsTreeMin[1], stampAabbMin.y);
            _ = atomicMin(&gsTreeMin[2], stampAabbMin.z);
            _ = atomicMax(&gsTreeMax[0], stampAabbMax.x);
            _ = atomicMax(&gsTreeMax[1], stampAabbMax.y);
            _ = atomicMax(&gsTreeMax[2], stampAabbMax.z);
        }
        workgroupBarrier();

        if (all(gtid == vec3<u32>(0u, 0u, 0u))) {
            let rootOffset = 16u * 16u * 16u + GPUSignedDistanceFields_BuildTreeMidNodeCount * 6u;
            if (atomicLoad(&gsTreeMin[0]) == 0xffffffffu) {
                StoreTreeCoord3(cascadeIndex, rootOffset + 0u, vec3<u32>(0u, 0u, 0u));
                StoreTreeCoord3(cascadeIndex, rootOffset + 3u, vec3<u32>(0u, 0u, 0u));
            } else {
                let localAabbMin = vec3<u32>(
                    atomicLoad(&gsTreeMin[0]),
                    atomicLoad(&gsTreeMin[1]),
                    atomicLoad(&gsTreeMin[2]));
                let localAabbMax = vec3<u32>(
                    atomicLoad(&gsTreeMax[0]),
                    atomicLoad(&gsTreeMax[1]),
                    atomicLoad(&gsTreeMax[2]));
                StoreTreeCoord3(cascadeIndex, rootOffset + 0u, localAabbMin);
                StoreTreeCoord3(cascadeIndex, rootOffset + 3u, localAabbMax);
            }
        }
    }
}
