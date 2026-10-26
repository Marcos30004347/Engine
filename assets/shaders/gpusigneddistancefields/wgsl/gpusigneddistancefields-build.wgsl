#include "gpusigneddistancefields_common.wgsl"

struct BuildDispatchUniforms {
    cascade : CascadeInfoGPU,
    pathIndex : u32,
    cascadeIndex : u32,
    cascadeCount : u32,
    dispatchAllCascades : u32,
    numJobs : u32,
    performSkinning : u32,
    doInitialization : u32,
    totalBricks : u32,
    maxReferences : u32,
    maxBricksPerBake : u32,
    atlasBricksPerAxis : u32,
    numJobVoxels : u32,
    maxTriangleSwapWords : u32,
    maxJobsPerCascade : u32,
    maxJobScanGroups : u32,
    maxVoxelScanGroups : u32,
    surfaceCachePageResolution : u32,
    surfaceCachePagesPerRow : u32,
    _padding3 : u32,
    _padding4 : u32,
}

@group(0) @binding(0) var<uniform> gUniforms : BuildDispatchUniforms;
@group(0) @binding(1) var<storage, read> CascadeInfos : array<CascadeInfoGPU>;
@group(0) @binding(2) var<storage, read_write> CascadeBuildInfos : array<CascadeBuildInfoGPU>;
@group(0) @binding(3) var<storage, read> MeshTable : array<MeshTableEntryGPU>;
@group(0) @binding(4) var<storage, read> Vertices : array<VertexGPU>;
@group(0) @binding(5) var<storage, read> Indices : array<u32>;
@group(0) @binding(6) var<storage, read> BoneWeights : array<BoneWeightGPU>;
@group(0) @binding(7) var<storage, read> BoneMatrices : array<mat4x4<f32>>;
@group(0) @binding(8) var<storage, read> Instances : array<InstanceGPU>;
@group(0) @binding(9) var<storage, read> Jobs : array<JobGPU>;
@group(0) @binding(10) var<storage, read> JobSubvoxelOffsets : array<u32>;
@group(0) @binding(11) var<storage, read_write> ScratchCounters : array<atomic<u32>>;
@group(0) @binding(12) var<storage, read_write> ScratchJobCounters : array<atomic<u32>>;
@group(0) @binding(13) var<storage, read_write> ScratchIndexSwap : array<u32>;
@group(0) @binding(14) var<storage, read_write> ScratchRawReferences : array<RawTriangleReferenceGPU>;
@group(0) @binding(15) var<storage, read_write> ScratchCompactedReferences : array<u32>;
@group(0) @binding(16) var<storage, read_write> ScratchReferenceCounters : array<atomic<u32>>;
@group(0) @binding(17) var<storage, read_write> ScratchVoxelAllocationFailCounters : array<u32>;
@group(0) @binding(18) var<storage, read_write> ScratchJobCountersScan : array<u32>;
@group(0) @binding(19) var<storage, read_write> ScratchJobGlobalCountersScan : array<u32>;
@group(0) @binding(20) var<storage, read_write> ScratchReferenceCounterScan : array<u32>;
@group(0) @binding(21) var<storage, read_write> ScratchReferenceGlobalScan : array<u32>;
@group(0) @binding(22) var<storage, read_write> ScratchStampScan : array<u32>;
@group(0) @binding(23) var<storage, read_write> ScratchStampGlobalScan : array<u32>;
@group(0) @binding(24) var<storage, read_write> ScratchBrickStorage : array<atomic<u32>>;
@group(0) @binding(25) var<storage, read_write> ScratchBrickStorageOffsets : array<u32>;
@group(0) @binding(26) var<storage, read_write> ScratchBrickCompressionList : array<u32>;
@group(0) @binding(27) var<storage, read_write> ScratchBrickClearList : array<u32>;
@group(0) @binding(28) var<storage, read_write> IndirectArgs : array<atomic<u32>>;
@group(0) @binding(29) var<storage, read_write> ContextIndirectArgs : array<u32>;
@group(0) @binding(30) var<storage, read_write> CascadeBrickMap : array<u32>;
@group(0) @binding(31) var<storage, read_write> ContextBrickVoxelMap : array<u32>;
@group(0) @binding(32) var<storage, read_write> ContextBrickAabb : array<u32>;
@group(0) @binding(33) var<storage, read_write> ContextBrickFreeList : array<u32>;
@group(0) @binding(34) var<storage, read_write> ContextBrickClearList : array<u32>;
@group(0) @binding(35) var<storage, read_write> ContextBrickDirtyList : array<u32>;
@group(0) @binding(36) var<storage, read_write> ContextBrickMergeList : array<u32>;
@group(0) @binding(37) var<storage, read_write> ContextBrickEikonalCounters : array<u32>;
@group(0) @binding(38) var<storage, read_write> ContextCounters : array<atomic<u32>>;
@group(0) @binding(39) var SdfAtlas : texture_storage_3d<r32float, read_write>;
@group(0) @binding(40) var<storage, read> SurfaceMaterials : array<SurfaceMaterialGPU>;
@group(0) @binding(44) var SurfaceCacheMaterialReferenceAtlas : texture_storage_2d<rgba32uint, write>;
@group(0) @binding(45) var<storage, read_write> SurfaceCacheMaterialReferenceScores : array<atomic<u32>>;
@group(0) @binding(46) var SurfaceCacheMaterialReferenceBasisAtlas : texture_storage_2d<rgba32uint, write>;

var<workgroup> gsScanBuffer : array<u32, GPUSignedDistanceFields_ScanGroupSize>;
var<workgroup> gsScanGroupId : atomic<u32>;
var<workgroup> gsScanCarry0 : atomic<u32>;
var<workgroup> gsScanCarry1 : atomic<u32>;
var<workgroup> gsBrickAabbMin : array<atomic<u32>, 3>;
var<workgroup> gsBrickAabbMax : array<atomic<u32>, 3>;
var<workgroup> gsVoxelizerItemsRefCount : array<u32, GPUSignedDistanceFields_VoxelizerGroupSize>;
var<workgroup> gsVoxelizerItemsTriangleIdSwapOffsets : array<u32, GPUSignedDistanceFields_VoxelizerGroupSize>;
var<workgroup> gsVoxelizerItemsPack0 : array<u32, GPUSignedDistanceFields_VoxelizerGroupSize>;
var<workgroup> gsVoxelizerItemsPack1 : array<u32, GPUSignedDistanceFields_VoxelizerGroupSize>;
var<workgroup> gsVoxelizerItemCounter : atomic<u32>;
var<workgroup> gsVoxelizerRefCounter : atomic<u32>;
var<workgroup> gsVoxelizerRefOffset : atomic<u32>;
var<workgroup> gsTriangleOffsetGlobal : atomic<u32>;
var<workgroup> gsTriangleOffset : atomic<u32>;
var<workgroup> gsVoxelizerHasSpace : atomic<u32>;

fn BuildCascadeIndex(groupY: u32) -> u32 {
    return select(gUniforms.cascadeIndex, groupY, gUniforms.dispatchAllCascades != 0u);
}

fn LoadCascadeInfo(cascadeIndex: u32) -> CascadeInfoGPU {
    return CascadeInfos[cascadeIndex];
}

fn JobBase(cascadeIndex: u32) -> u32 {
    return cascadeIndex * gUniforms.maxJobsPerCascade;
}

fn VoxelBase(cascadeIndex: u32) -> u32 {
    return cascadeIndex * GPUSignedDistanceFields_CascadeVoxelCount;
}

fn ScratchCounterBase(cascadeIndex: u32) -> u32 {
    return cascadeIndex * GPUSignedDistanceFields_ScratchCounter_Count;
}

fn JobGlobalScanBase(cascadeIndex: u32) -> u32 {
    return cascadeIndex * gUniforms.maxJobScanGroups;
}

fn VoxelGlobalScanBase(cascadeIndex: u32) -> u32 {
    return cascadeIndex * gUniforms.maxVoxelScanGroups;
}

fn ReferenceCounterIndex(cascadeIndex: u32, voxelIndex: u32) -> u32 {
    return VoxelBase(cascadeIndex) + voxelIndex;
}

fn JobCounterIndex(cascadeIndex: u32, jobIndex: u32) -> u32 {
    return JobBase(cascadeIndex) + jobIndex;
}

fn JobScanIndex(cascadeIndex: u32, jobIndex: u32) -> u32 {
    return JobBase(cascadeIndex) + jobIndex;
}

fn JobGlobalScanIndex(cascadeIndex: u32, groupIndex: u32) -> u32 {
    return JobGlobalScanBase(cascadeIndex) + groupIndex;
}

fn ReferenceGlobalScanIndex(cascadeIndex: u32, groupIndex: u32) -> u32 {
    return VoxelGlobalScanBase(cascadeIndex) + groupIndex;
}

fn StampGlobalScanIndex(cascadeIndex: u32, groupIndex: u32) -> u32 {
    return VoxelGlobalScanBase(cascadeIndex) + groupIndex;
}

fn RawReferenceIndex(cascadeIndex: u32, referenceIndex: u32) -> u32 {
    return cascadeIndex * gUniforms.maxReferences + referenceIndex;
}

fn CompactedReferenceIndex(cascadeIndex: u32, referenceIndex: u32) -> u32 {
    return cascadeIndex * gUniforms.maxReferences + referenceIndex;
}

fn IndexSwapWordIndex(cascadeIndex: u32, wordIndex: u32) -> u32 {
    return cascadeIndex * gUniforms.maxTriangleSwapWords + wordIndex;
}

fn BrickStorageWordBase(cascadeIndex: u32) -> u32 {
    return cascadeIndex * gUniforms.maxBricksPerBake * GPUSignedDistanceFields_BrickCellCount;
}

fn BrickStorageOffsetIndex(cascadeIndex: u32, brickId: u32) -> u32 {
    return cascadeIndex * gUniforms.totalBricks + brickId;
}

fn BrickListIndex(cascadeIndex: u32, itemIndex: u32) -> u32 {
    return cascadeIndex * gUniforms.maxBricksPerBake + itemIndex;
}

fn SurfaceCachePageTexelCount() -> u32 {
    return gUniforms.surfaceCachePageResolution * gUniforms.surfaceCachePageResolution;
}

fn SurfaceCachePageTexelFromBrickLocal(faceIndex: u32, brickLocal: vec3<f32>) -> vec2<u32> {
    let pageResolution = max(gUniforms.surfaceCachePageResolution, 1u);
    let faceUv = clamp(
        GPUSignedDistanceFieldsSurfaceCacheFaceUv(faceIndex, clamp(brickLocal, vec3<f32>(0.0), vec3<f32>(0.999999))),
        vec2<f32>(0.0),
        vec2<f32>(0.999999));
    return min(vec2<u32>(faceUv * f32(pageResolution)), vec2<u32>(pageResolution - 1u));
}

fn StageIndirectWordIndex(byteOffset: u32) -> u32 {
    return byteOffset / 4u;
}

fn LoadBuildInfo(cascadeIndex: u32) -> CascadeBuildInfoGPU {
    return CascadeBuildInfos[cascadeIndex];
}

fn IsJobInvalidating(job: JobGPU) -> bool {
    return (job.flags & 1u) != 0u;
}

fn MarkBrickFree(brickId: u32) {
    ContextBrickVoxelMap[brickId] = GPUSignedDistanceFields_InvalidId;
}

fn LoadBrickVoxelId(brickId: u32) -> u32 {
    return ContextBrickVoxelMap[brickId];
}

fn IsBuildable(cascadeIndex: u32, voxelIndex: u32) -> bool {
    let cascade = LoadCascadeInfo(cascadeIndex);
    return CascadeBrickMap[VoxelBase(cascadeIndex) + GPUSignedDistanceFieldsWrapFlatCoords(cascade, voxelIndex)] == GPUSignedDistanceFields_UninitializedId;
}

fn ClearRefCounter(cascadeIndex: u32, voxelIndex: u32) {
    let index = ReferenceCounterIndex(cascadeIndex, voxelIndex);
    atomicStore(&ScratchReferenceCounters[index], 0u);
    ScratchVoxelAllocationFailCounters[index] = 0u;
}

fn MarkFailed(cascadeIndex: u32, voxelIndex: u32) {
    ScratchVoxelAllocationFailCounters[ReferenceCounterIndex(cascadeIndex, voxelIndex)] = 1u;
}

fn LoadJobTriangleCount(cascadeIndex: u32, jobIndex: u32) -> u32 {
    let buildInfo = LoadBuildInfo(cascadeIndex);
    if (jobIndex >= buildInfo.numJobs) {
        return 0u;
    }

    let job = Jobs[JobBase(cascadeIndex) + jobIndex];
    let instance = Instances[job.instanceIndex];
    return MeshTable[instance.meshIndex].triangleCount;
}

fn FetchVertexPositionWorld(vertexIndex: u32, instance: InstanceGPU, mesh: MeshTableEntryGPU) -> vec3<f32> {
    let vertex = Vertices[vertexIndex];
    var localPosition = vec4<f32>(vertex.position, 1.0);

    if (gUniforms.performSkinning != 0u &&
        (instance.flags & 1u) != 0u &&
        mesh.boneWeightsPerVertex != 0u &&
        instance.boneCount != 0u) {
        var skinnedPosition = vec4<f32>(0.0, 0.0, 0.0, 0.0);
        var totalWeight = 0.0;
        let influenceCount = min(mesh.boneWeightsPerVertex, 4u);
        for (var influenceIndex = 0u; influenceIndex < 4u; influenceIndex = influenceIndex + 1u) {
            if (influenceIndex >= influenceCount) {
                break;
            }

            let boneWeight = BoneWeights[vertex.boneWeightOffset + influenceIndex];
            if (boneWeight.weight <= 0.0 || boneWeight.boneIndex >= instance.boneCount) {
                continue;
            }

            skinnedPosition = skinnedPosition + (BoneMatrices[instance.boneMatrixOffset + boneWeight.boneIndex] * localPosition) * boneWeight.weight;
            totalWeight = totalWeight + boneWeight.weight;
        }

        if (totalWeight > 1.0e-5) {
            localPosition = skinnedPosition;
        }
    }

    return (instance.modelMatrix * localPosition).xyz;
}

fn FetchSurfaceMaterial(mesh: MeshTableEntryGPU) -> SurfaceMaterialGPU {
    return SurfaceMaterials[mesh.surfaceMaterialIndex];
}

fn SurfaceCachePickTriangleMaterialId(materialId0: u32, materialId1: u32, materialId2: u32) -> u32 {
    if (materialId0 == materialId1 || materialId0 == materialId2) {
        return materialId0;
    }
    if (materialId1 == materialId2) {
        return materialId1;
    }
    return materialId0;
}

fn SurfaceCacheFetchTriangleSurfaceByIds(cascadeIndex: u32, jobIndex: u32, triangleIndex: u32, fallbackMaterialIndex: u32) -> SurfaceMaterialGPU {
    let job = Jobs[JobBase(cascadeIndex) + jobIndex];
    let instance = Instances[job.instanceIndex];
    let mesh = MeshTable[instance.meshIndex];
    let baseIndex = mesh.indexOffset + triangleIndex * 3u;
    let v0 = Vertices[mesh.vertexOffset + Indices[baseIndex + 0u]];
    let v1 = Vertices[mesh.vertexOffset + Indices[baseIndex + 1u]];
    let v2 = Vertices[mesh.vertexOffset + Indices[baseIndex + 2u]];

    var material : SurfaceMaterialGPU;
    material.baseColorOpacity = (v0.surfaceBaseColorOpacity + v1.surfaceBaseColorOpacity + v2.surfaceBaseColorOpacity) / 3.0;
    material.normalRoughness = (v0.surfaceNormalRoughness + v1.surfaceNormalRoughness + v2.surfaceNormalRoughness) / 3.0;
    material.emissiveMetallic = (v0.surfaceEmissiveMetallic + v1.surfaceEmissiveMetallic + v2.surfaceEmissiveMetallic) / 3.0;
    material.flags = v0.surfaceFlags | v1.surfaceFlags | v2.surfaceFlags;
    material.flags.x = SurfaceCachePickTriangleMaterialId(v0.surfaceFlags.x, v1.surfaceFlags.x, v2.surfaceFlags.x);
    return material;
}

fn SurfaceCacheFetchTriangleUVByIds(cascadeIndex: u32, jobIndex: u32, triangleIndex: u32, bary: vec3<f32>) -> vec2<f32> {
    let job = Jobs[JobBase(cascadeIndex) + jobIndex];
    let instance = Instances[job.instanceIndex];
    let mesh = MeshTable[instance.meshIndex];
    let baseIndex = mesh.indexOffset + triangleIndex * 3u;
    let v0 = Vertices[mesh.vertexOffset + Indices[baseIndex + 0u]];
    let v1 = Vertices[mesh.vertexOffset + Indices[baseIndex + 1u]];
    let v2 = Vertices[mesh.vertexOffset + Indices[baseIndex + 2u]];
    return v0.uvMaterial.xy * bary.x + v1.uvMaterial.xy * bary.y + v2.uvMaterial.xy * bary.z;
}

fn SurfaceCacheFetchTriangleUVExtentByIds(cascadeIndex: u32, jobIndex: u32, triangleIndex: u32) -> vec2<f32> {
    let job = Jobs[JobBase(cascadeIndex) + jobIndex];
    let instance = Instances[job.instanceIndex];
    let mesh = MeshTable[instance.meshIndex];
    let baseIndex = mesh.indexOffset + triangleIndex * 3u;
    let uv0 = Vertices[mesh.vertexOffset + Indices[baseIndex + 0u]].uvMaterial.xy;
    let uv1 = Vertices[mesh.vertexOffset + Indices[baseIndex + 1u]].uvMaterial.xy;
    let uv2 = Vertices[mesh.vertexOffset + Indices[baseIndex + 2u]].uvMaterial.xy;
    return max(abs(uv1 - uv0), max(abs(uv2 - uv0), abs(uv2 - uv1)));
}

fn SurfaceCacheBarycentric(point: vec3<f32>, a: vec3<f32>, b: vec3<f32>, c: vec3<f32>) -> vec3<f32> {
    let v0 = b - a;
    let v1 = c - a;
    let v2 = point - a;
    let d00 = dot(v0, v0);
    let d01 = dot(v0, v1);
    let d11 = dot(v1, v1);
    let d20 = dot(v2, v0);
    let d21 = dot(v2, v1);
    let denom = d00 * d11 - d01 * d01;
    if (abs(denom) <= 1.0e-12) {
        return vec3<f32>(1.0 / 3.0);
    }
    let v = (d11 * d20 - d01 * d21) / denom;
    let w = (d00 * d21 - d01 * d20) / denom;
    return vec3<f32>(1.0 - v - w, v, w);
}

fn SurfaceCacheFetchTriangleSurface(cascadeIndex: u32, tri: TriangleData) -> SurfaceMaterialGPU {
    return SurfaceCacheFetchTriangleSurfaceByIds(cascadeIndex, tri.jobIndex, tri.triangleIndex, tri.surfaceMaterialIndex);
}

fn SurfaceCacheFetchTrianglePartialSurface(cascadeIndex: u32, tri: TrianglePartial) -> SurfaceMaterialGPU {
    return SurfaceCacheFetchTriangleSurfaceByIds(cascadeIndex, tri.jobIndex, tri.triangleIndex, tri.surfaceMaterialIndex);
}

fn SurfaceCacheFetchTriangleCenterSurface(cascadeIndex: u32, center: TriangleCenter) -> SurfaceMaterialGPU {
    return SurfaceCacheFetchTriangleSurfaceByIds(cascadeIndex, center.jobIndex, center.triangleIndex, center.surfaceMaterialIndex);
}

fn SurfaceCacheQuantizeUvWindowSize(size: f32) -> u32 {
    let quantized = u32(ceil(clamp(abs(size), 0.0, 1.0) * 2047.0));
    return clamp(quantized, 1u, 2047u);
}

fn SurfaceCachePackUvWindow(uvWindowSize: vec2<f32>) -> u32 {
    let width = SurfaceCacheQuantizeUvWindowSize(uvWindowSize.x);
    let height = SurfaceCacheQuantizeUvWindowSize(uvWindowSize.y);
    let signX = select(0u, 1u << 22u, uvWindowSize.x < 0.0);
    let signY = select(0u, 1u << 23u, uvWindowSize.y < 0.0);
    return width | (height << 11u) | signX | signY | (1u << 31u);
}

fn SurfaceCacheStoreMaterialReference(
    brickId: u32,
    faceIndex: u32,
    pageTexel: vec2<u32>,
    materialId: u32,
    uv: vec2<f32>,
    uvWindowSize: vec2<f32>,
    uvBasis: vec4<f32>,
    score: u32) {
    let atlasTexel = GPUSignedDistanceFieldsSurfaceCacheTexel(
        brickId,
        faceIndex,
        pageTexel,
        gUniforms.surfaceCachePageResolution,
        gUniforms.surfaceCachePagesPerRow);
    let atlasWidth = max(gUniforms.surfaceCachePagesPerRow, 1u) * max(gUniforms.surfaceCachePageResolution, 1u);
    let scoreIndex = atlasTexel.x + atlasTexel.y * atlasWidth;
    let previousScore = atomicMax(&SurfaceCacheMaterialReferenceScores[scoreIndex], score);
    if (score <= previousScore) {
        return;
    }
    textureStore(
        SurfaceCacheMaterialReferenceAtlas,
        vec2<i32>(atlasTexel),
        vec4<u32>(materialId, bitcast<u32>(uv.x), bitcast<u32>(uv.y), SurfaceCachePackUvWindow(uvWindowSize)));
    textureStore(
        SurfaceCacheMaterialReferenceBasisAtlas,
        vec2<i32>(atlasTexel),
        vec4<u32>(
            bitcast<u32>(uvBasis.x),
            bitcast<u32>(uvBasis.y),
            bitcast<u32>(uvBasis.z),
            bitcast<u32>(uvBasis.w)));
}

fn SurfaceCachePointTriangleDistanceSquared(point: vec3<f32>, a: vec3<f32>, b: vec3<f32>, c: vec3<f32>) -> f32 {
    let ab = b - a;
    let ac = c - a;
    let ap = point - a;
    let d1 = dot(ab, ap);
    let d2 = dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0) {
        return dot(ap, ap);
    }

    let bp = point - b;
    let d3 = dot(ab, bp);
    let d4 = dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3) {
        return dot(bp, bp);
    }

    let vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
        let v = d1 / max(d1 - d3, 1.0e-20);
        let closest = a + ab * v;
        let delta = point - closest;
        return dot(delta, delta);
    }

    let cp = point - c;
    let d5 = dot(ab, cp);
    let d6 = dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6) {
        return dot(cp, cp);
    }

    let vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
        let w = d2 / max(d2 - d6, 1.0e-20);
        let closest = a + ac * w;
        let delta = point - closest;
        return dot(delta, delta);
    }

    let va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
        let w = (d4 - d3) / max((d4 - d3) + (d5 - d6), 1.0e-20);
        let closest = b + (c - b) * w;
        let delta = point - closest;
        return dot(delta, delta);
    }

    let normal = cross(ab, ac);
    let planeDistance = dot(point - a, normal);
    return planeDistance * planeDistance / max(dot(normal, normal), 1.0e-20);
}

fn SurfaceCacheSurfaceDistanceScore(surfaceDistance: f32) -> u32 {
    let packedDistance = u32(round(clamp(surfaceDistance, 0.0, 1.0) * 536870912.0));
    return max(1u, 536870912u - packedDistance);
}

fn SurfaceCacheStoreMaterialReferenceFootprint(
    brickId: u32,
    faceIndex: u32,
    pageTexel: vec2<u32>,
    materialId: u32,
    uvRangeOrigin: vec2<f32>,
    uvRangeSize: vec2<f32>,
    uvBasis: vec4<f32>,
    baseScore: u32,
    triangleP0: vec3<f32>,
    triangleP1: vec3<f32>,
    triangleP2: vec3<f32>,
    samplePosition: vec3<f32>,
    brixelSize: f32,
    brickLocal: vec3<f32>) {
    let pageResolution = max(gUniforms.surfaceCachePageResolution, 1u);
    let maxTexel = i32(pageResolution) - 1i;
    let radius = select(0i, 1i, pageResolution > 1u);
    let center = vec2<i32>(pageTexel);
    let depth = SurfaceCacheFaceDepthCoordinate(faceIndex, brickLocal);
    let brickLocalToWorld = 7.0 * brixelSize;
    for (var y = -radius; y <= radius; y = y + 1i) {
        for (var x = -radius; x <= radius; x = x + 1i) {
            let texel = center + vec2<i32>(x, y);
            if (all(texel >= vec2<i32>(0i)) && all(texel <= vec2<i32>(maxTexel))) {
                let texelCenterUv = (vec2<f32>(texel) + vec2<f32>(0.5)) / f32(pageResolution);
                let texelBrickLocal = SurfaceCacheBrickLocalFromFaceUv(faceIndex, texelCenterUv, depth);
                let texelPosition = samplePosition + (texelBrickLocal - brickLocal) * brickLocalToWorld;
                let texelDistance = SurfaceCachePointTriangleDistanceSquared(texelPosition, triangleP0, triangleP1, triangleP2) * 0.25 * 0.25;
                let texelScore = baseScore + SurfaceCacheSurfaceDistanceScore(texelDistance);
                let texelUvOrigin = uvRangeOrigin + uvBasis.xy * f32(x) + uvBasis.zw * f32(y);
                SurfaceCacheStoreMaterialReference(brickId, faceIndex, vec2<u32>(texel), materialId, texelUvOrigin, uvRangeSize, uvBasis, texelScore);
            }
        }
    }
}

fn SurfaceCacheFaceDepthCoordinate(faceIndex: u32, brickLocal: vec3<f32>) -> f32 {
    switch (faceIndex) {
        case GPUSignedDistanceFields_SurfaceCacheFace_PosX: { return brickLocal.x; }
        case GPUSignedDistanceFields_SurfaceCacheFace_NegX: { return brickLocal.x; }
        case GPUSignedDistanceFields_SurfaceCacheFace_PosY: { return brickLocal.y; }
        case GPUSignedDistanceFields_SurfaceCacheFace_NegY: { return brickLocal.y; }
        default: { return brickLocal.z; }
    }
}

fn SurfaceCacheBrickLocalFromFaceUv(faceIndex: u32, faceUv: vec2<f32>, depth: f32) -> vec3<f32> {
    switch (faceIndex) {
        case GPUSignedDistanceFields_SurfaceCacheFace_PosX: { return vec3<f32>(depth, faceUv.y, faceUv.x); }
        case GPUSignedDistanceFields_SurfaceCacheFace_NegX: { return vec3<f32>(depth, faceUv.y, 1.0 - faceUv.x); }
        case GPUSignedDistanceFields_SurfaceCacheFace_PosY: { return vec3<f32>(faceUv.x, depth, faceUv.y); }
        case GPUSignedDistanceFields_SurfaceCacheFace_NegY: { return vec3<f32>(faceUv.x, depth, 1.0 - faceUv.y); }
        case GPUSignedDistanceFields_SurfaceCacheFace_PosZ: { return vec3<f32>(faceUv.x, faceUv.y, depth); }
        default: { return vec3<f32>(1.0 - faceUv.x, faceUv.y, depth); }
    }
}

fn SurfaceCacheFetchTriangleUVAtPosition(
    cascadeIndex: u32,
    jobIndex: u32,
    triangleIndex: u32,
    triangleP0: vec3<f32>,
    triangleP1: vec3<f32>,
    triangleP2: vec3<f32>,
    position: vec3<f32>) -> vec2<f32> {
    let bary = clamp(SurfaceCacheBarycentric(position, triangleP0, triangleP1, triangleP2), vec3<f32>(0.0), vec3<f32>(1.0));
    let barySum = max(bary.x + bary.y + bary.z, 1.0e-6);
    return SurfaceCacheFetchTriangleUVByIds(cascadeIndex, jobIndex, triangleIndex, bary / barySum);
}

fn SurfaceCacheUnwrapUv(baseUv: vec2<f32>, uv: vec2<f32>) -> vec2<f32> {
    let delta = uv - baseUv;
    return baseUv + delta - round(delta);
}

struct SurfaceCacheTriangleUvBasis {
    origin : vec2<f32>,
    basisX : vec2<f32>,
    basisY : vec2<f32>,
    windowSize : vec2<f32>,
}

fn SurfaceCacheTriangleUvBasisForFaceTexel(
    cascadeIndex: u32,
    jobIndex: u32,
    triangleIndex: u32,
    triangleP0: vec3<f32>,
    triangleP1: vec3<f32>,
    triangleP2: vec3<f32>,
    samplePosition: vec3<f32>,
    brixelSize: f32,
    brickLocal: vec3<f32>,
    faceIndex: u32,
    pageTexel: vec2<u32>) -> SurfaceCacheTriangleUvBasis {
    let pageResolution = max(gUniforms.surfaceCachePageResolution, 1u);
    let faceUvMin = vec2<f32>(pageTexel) / f32(pageResolution);
    let faceUvMax = vec2<f32>(pageTexel + vec2<u32>(1u)) / f32(pageResolution);
    let depth = SurfaceCacheFaceDepthCoordinate(faceIndex, brickLocal);
    let brickLocalOrigin = SurfaceCacheBrickLocalFromFaceUv(faceIndex, faceUvMin, depth);
    let brickLocalX = SurfaceCacheBrickLocalFromFaceUv(faceIndex, vec2<f32>(faceUvMax.x, faceUvMin.y), depth);
    let brickLocalY = SurfaceCacheBrickLocalFromFaceUv(faceIndex, vec2<f32>(faceUvMin.x, faceUvMax.y), depth);
    let brickLocalXY = SurfaceCacheBrickLocalFromFaceUv(faceIndex, faceUvMax, depth);
    let brickLocalToWorld = 7.0 * brixelSize;
    let positionOrigin = samplePosition + (brickLocalOrigin - brickLocal) * brickLocalToWorld;
    let positionX = samplePosition + (brickLocalX - brickLocal) * brickLocalToWorld;
    let positionY = samplePosition + (brickLocalY - brickLocal) * brickLocalToWorld;
    let positionXY = samplePosition + (brickLocalXY - brickLocal) * brickLocalToWorld;
    let uvOrigin = SurfaceCacheFetchTriangleUVAtPosition(cascadeIndex, jobIndex, triangleIndex, triangleP0, triangleP1, triangleP2, positionOrigin);
    let uvX = SurfaceCacheUnwrapUv(uvOrigin, SurfaceCacheFetchTriangleUVAtPosition(cascadeIndex, jobIndex, triangleIndex, triangleP0, triangleP1, triangleP2, positionX));
    let uvY = SurfaceCacheUnwrapUv(uvOrigin, SurfaceCacheFetchTriangleUVAtPosition(cascadeIndex, jobIndex, triangleIndex, triangleP0, triangleP1, triangleP2, positionY));
    let uvXY = SurfaceCacheUnwrapUv(uvOrigin, SurfaceCacheFetchTriangleUVAtPosition(cascadeIndex, jobIndex, triangleIndex, triangleP0, triangleP1, triangleP2, positionXY));

    let triangleUv0 = SurfaceCacheUnwrapUv(uvOrigin, SurfaceCacheFetchTriangleUVByIds(cascadeIndex, jobIndex, triangleIndex, vec3<f32>(1.0, 0.0, 0.0)));
    let triangleUv1 = SurfaceCacheUnwrapUv(uvOrigin, SurfaceCacheFetchTriangleUVByIds(cascadeIndex, jobIndex, triangleIndex, vec3<f32>(0.0, 1.0, 0.0)));
    let triangleUv2 = SurfaceCacheUnwrapUv(uvOrigin, SurfaceCacheFetchTriangleUVByIds(cascadeIndex, jobIndex, triangleIndex, vec3<f32>(0.0, 0.0, 1.0)));

    let texelUvMin = min(uvOrigin, min(uvX, min(uvY, uvXY)));
    let texelUvMax = max(uvOrigin, max(uvX, max(uvY, uvXY)));
    let triangleUvMin = min(triangleUv0, min(triangleUv1, triangleUv2));
    let triangleUvMax = max(triangleUv0, max(triangleUv1, triangleUv2));
    let clippedUvMin = max(texelUvMin, triangleUvMin);
    let clippedUvMax = min(texelUvMax, triangleUvMax);
    let unclippedWindowSize = texelUvMax - texelUvMin;
    let clippedWindowSize = clippedUvMax - clippedUvMin;
    let windowSize = select(unclippedWindowSize, clippedWindowSize, all(clippedWindowSize >= vec2<f32>(0.0)));

    return SurfaceCacheTriangleUvBasis(
        uvOrigin,
        uvX - uvOrigin,
        uvY - uvOrigin,
        max(abs(windowSize), vec2<f32>(1.0 / 2047.0)));
}

fn SurfaceCacheNormalSignedFace(axis: u32, normal: vec3<f32>) -> u32 {
    if (axis == 0u) {
        return select(GPUSignedDistanceFields_SurfaceCacheFace_NegX, GPUSignedDistanceFields_SurfaceCacheFace_PosX, normal.x >= 0.0);
    }
    if (axis == 1u) {
        return select(GPUSignedDistanceFields_SurfaceCacheFace_NegY, GPUSignedDistanceFields_SurfaceCacheFace_PosY, normal.y >= 0.0);
    }
    return select(GPUSignedDistanceFields_SurfaceCacheFace_NegZ, GPUSignedDistanceFields_SurfaceCacheFace_PosZ, normal.z >= 0.0);
}

fn SurfaceCacheStoreMaterialReferenceFace(
    cascadeIndex: u32,
    brickId: u32,
    brickLocal: vec3<f32>,
    faceIndex: u32,
    materialId: u32,
    jobIndex: u32,
    triangleIndex: u32,
    triangleP0: vec3<f32>,
    triangleP1: vec3<f32>,
    triangleP2: vec3<f32>,
    samplePosition: vec3<f32>,
    brixelSize: f32,
    score: u32) {
    let pageTexel = SurfaceCachePageTexelFromBrickLocal(faceIndex, brickLocal);
    let uvBasis = SurfaceCacheTriangleUvBasisForFaceTexel(
        cascadeIndex,
        jobIndex,
        triangleIndex,
        triangleP0,
        triangleP1,
        triangleP2,
        samplePosition,
        brixelSize,
        brickLocal,
        faceIndex,
        pageTexel);
    let packedBasis = vec4<f32>(uvBasis.basisX, uvBasis.basisY);
    SurfaceCacheStoreMaterialReferenceFootprint(
        brickId,
        faceIndex,
        pageTexel,
        materialId,
        uvBasis.origin,
        uvBasis.windowSize,
        packedBasis,
        score,
        triangleP0,
        triangleP1,
        triangleP2,
        samplePosition,
        brixelSize,
        brickLocal);
}

fn SurfaceCacheStoreMaterialReferenceNormalFaces(
    cascadeIndex: u32,
    brickId: u32,
    brickLocal: vec3<f32>,
    surfaceNormal: vec3<f32>,
    materialId: u32,
    jobIndex: u32,
    triangleIndex: u32,
    triangleP0: vec3<f32>,
    triangleP1: vec3<f32>,
    triangleP2: vec3<f32>,
    samplePosition: vec3<f32>,
    brixelSize: f32,
    score: u32) {
    let absNormal = abs(surfaceNormal);
    let maxComponent = max(absNormal.x, max(absNormal.y, absNormal.z));
    if (maxComponent <= 1.0e-6) {
        return;
    }

    let threshold = max(maxComponent * 0.35, 1.0e-4);
    if (absNormal.x >= threshold) {
        SurfaceCacheStoreMaterialReferenceFace(cascadeIndex, brickId, brickLocal, SurfaceCacheNormalSignedFace(0u, surfaceNormal), materialId, jobIndex, triangleIndex, triangleP0, triangleP1, triangleP2, samplePosition, brixelSize, score);
    }
    if (absNormal.y >= threshold) {
        SurfaceCacheStoreMaterialReferenceFace(cascadeIndex, brickId, brickLocal, SurfaceCacheNormalSignedFace(1u, surfaceNormal), materialId, jobIndex, triangleIndex, triangleP0, triangleP1, triangleP2, samplePosition, brixelSize, score);
    }
    if (absNormal.z >= threshold) {
        SurfaceCacheStoreMaterialReferenceFace(cascadeIndex, brickId, brickLocal, SurfaceCacheNormalSignedFace(2u, surfaceNormal), materialId, jobIndex, triangleIndex, triangleP0, triangleP1, triangleP2, samplePosition, brixelSize, score);
    }
}

fn SurfaceCacheTriangleUvWindowSize(
    triangleP0: vec3<f32>,
    triangleP1: vec3<f32>,
    triangleP2: vec3<f32>,
    uv0: vec2<f32>,
    uv1: vec2<f32>,
    uv2: vec2<f32>,
    brixelSize: f32) -> vec2<f32> {
    let edge0 = triangleP1 - triangleP0;
    let edge1 = triangleP2 - triangleP0;
    let normal = cross(edge0, edge1);
    let area2 = dot(normal, normal);
    let uvExtent = max(abs(uv1 - uv0), max(abs(uv2 - uv0), abs(uv2 - uv1)));
    if (area2 <= 1.0e-12) {
        return max(uvExtent, vec2<f32>(1.0 / 2047.0));
    }

    let uvEdge0 = uv1 - uv0;
    let uvEdge1 = uv2 - uv0;
    let gradU = (uvEdge0.x * cross(edge1, normal) + uvEdge1.x * cross(normal, edge0)) / area2;
    let gradV = (uvEdge0.y * cross(edge1, normal) + uvEdge1.y * cross(normal, edge0)) / area2;
    return max(vec2<f32>(length(gradU), length(gradV)) * max(brixelSize, 1.0e-6), vec2<f32>(1.0 / 2047.0));
}

fn SurfaceCacheTriangleUvWindowSizeByIds(
    cascadeIndex: u32,
    jobIndex: u32,
    triangleIndex: u32,
    triangleP0: vec3<f32>,
    triangleP1: vec3<f32>,
    triangleP2: vec3<f32>,
    brixelSize: f32) -> vec2<f32> {
    let job = Jobs[JobBase(cascadeIndex) + jobIndex];
    let instance = Instances[job.instanceIndex];
    let mesh = MeshTable[instance.meshIndex];
    let baseIndex = mesh.indexOffset + triangleIndex * 3u;
    let uv0 = Vertices[mesh.vertexOffset + Indices[baseIndex + 0u]].uvMaterial.xy;
    let uv1 = Vertices[mesh.vertexOffset + Indices[baseIndex + 1u]].uvMaterial.xy;
    let uv2 = Vertices[mesh.vertexOffset + Indices[baseIndex + 2u]].uvMaterial.xy;
    return SurfaceCacheTriangleUvWindowSize(triangleP0, triangleP1, triangleP2, uv0, uv1, uv2, brixelSize);
}

fn SurfaceCacheClearMaterialReferenceTexel(brickId: u32, surfaceTexelIndex: u32) {
    if (gUniforms.surfaceCachePageResolution == 0u || !GPUSignedDistanceFieldsIsBrickValid(brickId)) {
        return;
    }

    let pageTexelCount = SurfaceCachePageTexelCount();
    if (surfaceTexelIndex >= GPUSignedDistanceFields_SurfaceCacheFaceCount * pageTexelCount) {
        return;
    }

    let faceIndex = surfaceTexelIndex / pageTexelCount;
    let pageTexelIndex = surfaceTexelIndex - faceIndex * pageTexelCount;
    let pageResolution = gUniforms.surfaceCachePageResolution;
    let pageTexel = vec2<u32>(pageTexelIndex % pageResolution, pageTexelIndex / pageResolution);
    let atlasTexel = GPUSignedDistanceFieldsSurfaceCacheTexel(brickId, faceIndex, pageTexel, pageResolution, gUniforms.surfaceCachePagesPerRow);
    let atlasWidth = max(gUniforms.surfaceCachePagesPerRow, 1u) * max(pageResolution, 1u);
    atomicStore(&SurfaceCacheMaterialReferenceScores[atlasTexel.x + atlasTexel.y * atlasWidth], 0u);
    textureStore(
        SurfaceCacheMaterialReferenceAtlas,
        vec2<i32>(atlasTexel),
        vec4<u32>(0u));
    textureStore(
        SurfaceCacheMaterialReferenceBasisAtlas,
        vec2<i32>(atlasTexel),
        vec4<u32>(0u));
}

fn SurfaceCacheAccumulateTriangleMaterial(
    cascadeIndex: u32,
    brickId: u32,
    material: SurfaceMaterialGPU,
    jobIndex: u32,
    triangleIndex: u32,
    triangleP0: vec3<f32>,
    triangleP1: vec3<f32>,
    triangleP2: vec3<f32>,
    samplePosition: vec3<f32>,
    surfaceDistance: f32,
    brixelSize: f32,
    surfaceNormal: vec3<f32>,
    brickLocal: vec3<f32>) {
    if (gUniforms.surfaceCachePageResolution == 0u || !GPUSignedDistanceFieldsIsBrickValid(brickId)) {
        return;
    }

    let score = SurfaceCacheSurfaceDistanceScore(surfaceDistance) >> 3u;
    SurfaceCacheStoreMaterialReferenceNormalFaces(
        cascadeIndex,
        brickId,
        brickLocal,
        surfaceNormal,
        material.flags.x,
        jobIndex,
        triangleIndex,
        triangleP0,
        triangleP1,
        triangleP2,
        samplePosition,
        brixelSize,
        score);
}

fn FetchTriangle(cascadeIndex: u32, triangleIndex: u32, jobIndex: u32) -> TriangleData {
    let cascade = LoadCascadeInfo(cascadeIndex);
    let job = Jobs[JobBase(cascadeIndex) + jobIndex];
    let instance = Instances[job.instanceIndex];
    let mesh = MeshTable[instance.meshIndex];
    let baseIndex = mesh.indexOffset + triangleIndex * 3u;

    var tri : TriangleData;
    tri.jobIndex = jobIndex;
    tri.triangleIndex = triangleIndex;
    tri.surfaceMaterialIndex = mesh.surfaceMaterialIndex;
    tri.wp0 = FetchVertexPositionWorld(mesh.vertexOffset + Indices[baseIndex + 0u], instance, mesh) - cascade.gridMin;
    tri.wp1 = FetchVertexPositionWorld(mesh.vertexOffset + Indices[baseIndex + 1u], instance, mesh) - cascade.gridMin;
    tri.wp2 = FetchVertexPositionWorld(mesh.vertexOffset + Indices[baseIndex + 2u], instance, mesh) - cascade.gridMin;
    return tri;
}

fn GetTriangleBounds(cascadeIndex: u32, jobIndex: u32, triangleIndex: u32) -> TriangleVoxelBounds {
    let cascade = LoadCascadeInfo(cascadeIndex);
    let tri = FetchTriangle(cascadeIndex, triangleIndex, jobIndex);
    let inflationSize = cascade.voxelSize / 7.0;
    var bounds : TriangleVoxelBounds;
    bounds.boundMin = min(tri.wp0, min(tri.wp1, tri.wp2));
    bounds.boundMax = max(tri.wp0, max(tri.wp1, tri.wp2));

    var alignedMin : vec3<f32>;
    alignedMin.x = select(bounds.boundMin.x - 1.0, bounds.boundMin.x, bounds.boundMin.x > 0.0);
    alignedMin.y = select(bounds.boundMin.y - 1.0, bounds.boundMin.y, bounds.boundMin.y > 0.0);
    alignedMin.z = select(bounds.boundMin.z - 1.0, bounds.boundMin.z, bounds.boundMin.z > 0.0);

    let voxelMinI = max(vec3<i32>(0, 0, 0), vec3<i32>((alignedMin - vec3<f32>(inflationSize)) / cascade.voxelSize));
    bounds.voxelMin = min(
        vec3<u32>(GPUSignedDistanceFields_CascadeResolution - 1u, GPUSignedDistanceFields_CascadeResolution - 1u, GPUSignedDistanceFields_CascadeResolution - 1u),
        vec3<u32>(voxelMinI));

    var alignedMax : vec3<f32>;
    alignedMax.x = select(bounds.boundMax.x - 1.0, bounds.boundMax.x, bounds.boundMax.x > 0.0);
    alignedMax.y = select(bounds.boundMax.y - 1.0, bounds.boundMax.y, bounds.boundMax.y > 0.0);
    alignedMax.z = select(bounds.boundMax.z - 1.0, bounds.boundMax.z, bounds.boundMax.z > 0.0);

    let voxelMaxI = max(vec3<i32>(0, 0, 0), vec3<i32>((alignedMax + vec3<f32>(inflationSize)) / cascade.voxelSize));
    bounds.voxelMax = min(
        vec3<u32>(GPUSignedDistanceFields_CascadeResolution - 1u, GPUSignedDistanceFields_CascadeResolution - 1u, GPUSignedDistanceFields_CascadeResolution - 1u),
        vec3<u32>(voxelMaxI)) + vec3<u32>(1u, 1u, 1u);

    let localGridMax = cascade.gridMax - cascade.gridMin;
    if (!(all(bounds.boundMin <= localGridMax + vec3<f32>(inflationSize)) && all(bounds.boundMax >= vec3<f32>(-inflationSize)))) {
        bounds.voxelMax = bounds.voxelMin;
    }
    return bounds;
}

fn StoreTriangleCenter(cascadeIndex: u32, triangleOffsetBytes: u32, tri: TriangleData) {
    let baseIndex = triangleOffsetBytes >> 2u;
    let center = (tri.wp0 + tri.wp1 + tri.wp2) / 3.0;
    ScratchIndexSwap[IndexSwapWordIndex(cascadeIndex, baseIndex + 0u)] = bitcast<u32>(center.x);
    ScratchIndexSwap[IndexSwapWordIndex(cascadeIndex, baseIndex + 1u)] = bitcast<u32>(center.y);
    ScratchIndexSwap[IndexSwapWordIndex(cascadeIndex, baseIndex + 2u)] = bitcast<u32>(center.z);
    ScratchIndexSwap[IndexSwapWordIndex(cascadeIndex, baseIndex + 3u)] = tri.surfaceMaterialIndex;
    ScratchIndexSwap[IndexSwapWordIndex(cascadeIndex, baseIndex + 4u)] = tri.jobIndex;
    ScratchIndexSwap[IndexSwapWordIndex(cascadeIndex, baseIndex + 5u)] = tri.triangleIndex;
}

fn LoadTriangleCenter(cascadeIndex: u32, triangleOffsetBytes: u32) -> TriangleCenter {
    let baseIndex = triangleOffsetBytes >> 2u;
    var center : TriangleCenter;
    center.center = vec3<f32>(
        bitcast<f32>(ScratchIndexSwap[IndexSwapWordIndex(cascadeIndex, baseIndex + 0u)]),
        bitcast<f32>(ScratchIndexSwap[IndexSwapWordIndex(cascadeIndex, baseIndex + 1u)]),
        bitcast<f32>(ScratchIndexSwap[IndexSwapWordIndex(cascadeIndex, baseIndex + 2u)]));
    center.surfaceMaterialIndex = ScratchIndexSwap[IndexSwapWordIndex(cascadeIndex, baseIndex + 3u)];
    center.jobIndex = ScratchIndexSwap[IndexSwapWordIndex(cascadeIndex, baseIndex + 4u)];
    center.triangleIndex = ScratchIndexSwap[IndexSwapWordIndex(cascadeIndex, baseIndex + 5u)];
    return center;
}

fn StoreTrianglePartial(cascadeIndex: u32, triangleOffsetBytes: u32, tri: TriangleData) {
    let e0 = tri.wp1 - tri.wp0;
    let e1 = tri.wp2 - tri.wp0;
    let baseIndex = triangleOffsetBytes >> 2u;
    ScratchIndexSwap[IndexSwapWordIndex(cascadeIndex, baseIndex + 0u)] = bitcast<u32>(tri.wp0.x);
    ScratchIndexSwap[IndexSwapWordIndex(cascadeIndex, baseIndex + 1u)] = bitcast<u32>(tri.wp0.y);
    ScratchIndexSwap[IndexSwapWordIndex(cascadeIndex, baseIndex + 2u)] = GPUSignedDistanceFieldsPackF16x2(vec2<f32>(e0.x, e0.y));
    ScratchIndexSwap[IndexSwapWordIndex(cascadeIndex, baseIndex + 3u)] = GPUSignedDistanceFieldsPackF16x2(vec2<f32>(e0.z, e1.x));
    ScratchIndexSwap[IndexSwapWordIndex(cascadeIndex, baseIndex + 4u)] = GPUSignedDistanceFieldsPackF16x2(vec2<f32>(e1.y, e1.z));
    ScratchIndexSwap[IndexSwapWordIndex(cascadeIndex, baseIndex + 5u)] = bitcast<u32>(tri.wp0.z);
    ScratchIndexSwap[IndexSwapWordIndex(cascadeIndex, baseIndex + 6u)] = tri.surfaceMaterialIndex;
    ScratchIndexSwap[IndexSwapWordIndex(cascadeIndex, baseIndex + 7u)] = tri.jobIndex;
    ScratchIndexSwap[IndexSwapWordIndex(cascadeIndex, baseIndex + 8u)] = tri.triangleIndex;
}

fn LoadTrianglePartial(cascadeIndex: u32, triangleOffsetBytes: u32) -> TrianglePartial {
    let baseIndex = triangleOffsetBytes >> 2u;
    let wp0xy = vec2<u32>(
        ScratchIndexSwap[IndexSwapWordIndex(cascadeIndex, baseIndex + 0u)],
        ScratchIndexSwap[IndexSwapWordIndex(cascadeIndex, baseIndex + 1u)]);
    let ed0 = vec2<u32>(
        ScratchIndexSwap[IndexSwapWordIndex(cascadeIndex, baseIndex + 2u)],
        ScratchIndexSwap[IndexSwapWordIndex(cascadeIndex, baseIndex + 3u)]);
    let ed1 = vec2<u32>(
        ScratchIndexSwap[IndexSwapWordIndex(cascadeIndex, baseIndex + 4u)],
        ScratchIndexSwap[IndexSwapWordIndex(cascadeIndex, baseIndex + 5u)]);

    var tri : TrianglePartial;
    tri.wp0 = vec3<f32>(bitcast<f32>(wp0xy.x), bitcast<f32>(wp0xy.y), bitcast<f32>(ed1.y));
    let e0xy = GPUSignedDistanceFieldsUnpackF16x2(ed0.x);
    let e0zE1x = GPUSignedDistanceFieldsUnpackF16x2(ed0.y);
    let e1yz = GPUSignedDistanceFieldsUnpackF16x2(ed1.x);
    tri.wp1 = tri.wp0 + vec3<f32>(e0xy.x, e0xy.y, e0zE1x.x);
    tri.wp2 = tri.wp0 + vec3<f32>(e0zE1x.y, e1yz.x, e1yz.y);
    tri.surfaceMaterialIndex = ScratchIndexSwap[IndexSwapWordIndex(cascadeIndex, baseIndex + 6u)];
    tri.jobIndex = ScratchIndexSwap[IndexSwapWordIndex(cascadeIndex, baseIndex + 7u)];
    tri.triangleIndex = ScratchIndexSwap[IndexSwapWordIndex(cascadeIndex, baseIndex + 8u)];
    return tri;
}

fn AddReferenceOrMarkVoxelFailed(cascadeIndex: u32, voxelIndex: u32, triangleId: u32) {
    if (!IsBuildable(cascadeIndex, voxelIndex)) {
        return;
    }

    let localRefIndex = atomicAdd(&ScratchReferenceCounters[ReferenceCounterIndex(cascadeIndex, voxelIndex)], 1u);
    var reference : RawTriangleReferenceGPU;
    reference.voxelIndex = voxelIndex;
    reference.triangleId = triangleId;
    reference.localReferenceIndex = localRefIndex;
    reference._padding0 = 0u;

    let coarseRefOffset = atomicAdd(&ScratchCounters[ScratchCounterBase(cascadeIndex) + GPUSignedDistanceFields_ScratchCounter_References], 1u);
    let maxReferences = atomicLoad(&ScratchCounters[ScratchCounterBase(cascadeIndex) + GPUSignedDistanceFields_ScratchCounter_MaxReferences]);
    if (coarseRefOffset < maxReferences) {
        ScratchRawReferences[RawReferenceIndex(cascadeIndex, coarseRefOffset)] = reference;
    } else {
        MarkFailed(cascadeIndex, voxelIndex);
    }
}

fn GetReferenceOffset(cascadeIndex: u32, voxelIndex: u32) -> u32 {
    let groupScanId = voxelIndex / GPUSignedDistanceFields_ScanGroupSize;
    return ScratchReferenceGlobalScan[ReferenceGlobalScanIndex(cascadeIndex, groupScanId)] +
        ScratchReferenceCounterScan[ReferenceCounterIndex(cascadeIndex, voxelIndex)];
}

fn StoreCRItem(index: u32, item: CRItem) {
    gsVoxelizerItemsPack0[index] = ((item.bounds_min.x & 0x3ffu) << 0u) |
        ((item.bounds_min.y & 0x3ffu) << 10u) |
        ((item.bounds_min.z & 0x3ffu) << 20u);
    gsVoxelizerItemsPack1[index] = ((item.bounds_max.x & 0x3ffu) << 0u) |
        ((item.bounds_max.y & 0x3ffu) << 10u) |
        ((item.bounds_max.z & 0x3ffu) << 20u);
}

fn LoadCRItem(index: u32) -> CRItem {
    let pack0 = gsVoxelizerItemsPack0[index];
    let pack1 = gsVoxelizerItemsPack1[index];
    var item : CRItem;
    item.bounds_min.x = (pack0 >> 0u) & 0x3ffu;
    item.bounds_min.y = (pack0 >> 10u) & 0x3ffu;
    item.bounds_min.z = (pack0 >> 20u) & 0x3ffu;
    item.bounds_max.x = (pack1 >> 0u) & 0x3ffu;
    item.bounds_max.y = (pack1 >> 10u) & 0x3ffu;
    item.bounds_max.z = (pack1 >> 20u) & 0x3ffu;
    return item;
}

fn LoadJobTriangleCountScan(cascadeIndex: u32, jobIndex: u32) -> u32 {
    return ScratchJobCountersScan[JobScanIndex(cascadeIndex, jobIndex)] +
        ScratchJobGlobalCountersScan[JobGlobalScanIndex(cascadeIndex, jobIndex / GPUSignedDistanceFields_ScanGroupSize)];
}

fn JobLowerBound(cascadeIndex: u32, triangleOffset: u32) -> u32 {
    var cursor = 0u;
    var size = LoadBuildInfo(cascadeIndex).numJobs;
    while (size > 0u) {
        let sizeHalf = size >> 1u;
        let mid = cursor + sizeHalf;
        if (LoadJobTriangleCountScan(cascadeIndex, mid) > triangleOffset) {
            size = sizeHalf;
        } else {
            cursor = mid + 1u;
            size = size - sizeHalf - 1u;
        }
    }
    return max(cursor, 1u) - 1u;
}

fn JobLowerBoundBySubvoxel(cascadeIndex: u32, subvoxelOffset: u32) -> u32 {
    var cursor = 0u;
    var size = LoadBuildInfo(cascadeIndex).numJobs;
    while (size > 0u) {
        let sizeHalf = size >> 1u;
        let mid = cursor + sizeHalf;
        if (JobSubvoxelOffsets[JobBase(cascadeIndex) + mid] > subvoxelOffset) {
            size = sizeHalf;
        } else {
            cursor = mid + 1u;
            size = size - sizeHalf - 1u;
        }
    }
    return max(cursor, 1u) - 1u;
}

fn StampLowerBound(cascadeIndex: u32, itemId: u32) -> u32 {
    var cursor = 0u;
    var size = GPUSignedDistanceFields_CascadeVoxelCount;
    while (size > 0u) {
        let sizeHalf = size >> 1u;
        let mid = cursor + sizeHalf;
        if (ScratchStampScan[ReferenceCounterIndex(cascadeIndex, mid)] > itemId) {
            size = sizeHalf;
        } else {
            cursor = mid + 1u;
            size = size - sizeHalf - 1u;
        }
    }
    return max(cursor, 1u) - 1u;
}

fn GroupScanExclusiveAdd(gid: u32, groupSize: u32) -> u32 {
    var sum = 0u;
    var stride = 1u;
    while (stride <= (groupSize >> 1u)) {
        if (gid < groupSize / (2u * stride)) {
            let dst = 2u * (gid + 1u) * stride - 1u;
            let src = (2u * gid + 1u) * stride - 1u;
            gsScanBuffer[dst] = gsScanBuffer[dst] + gsScanBuffer[src];
        }
        workgroupBarrier();
        stride = stride << 1u;
    }

    if (gid == 0u) {
        sum = gsScanBuffer[groupSize - 1u];
        gsScanBuffer[groupSize - 1u] = 0u;
    }
    workgroupBarrier();

    stride = groupSize >> 1u;
    while (stride > 0u) {
        if (gid < groupSize / (2u * stride)) {
            let leftIndex = (2u * gid + 1u) * stride - 1u;
            let rightIndex = 2u * (gid + 1u) * stride - 1u;
            let tmp = gsScanBuffer[leftIndex];
            gsScanBuffer[leftIndex] = gsScanBuffer[rightIndex];
            gsScanBuffer[rightIndex] = gsScanBuffer[rightIndex] + tmp;
        }
        workgroupBarrier();
        stride = stride >> 1u;
    }

    return sum;
}

fn LoadJobGroupTriangleSum(cascadeIndex: u32, groupIndex: u32) -> u32 {
    let buildInfo = LoadBuildInfo(cascadeIndex);
    let groupStart = groupIndex * GPUSignedDistanceFields_ScanGroupSize;
    if (groupStart >= buildInfo.numJobs) {
        return 0u;
    }

    let lastJobIndex = min(groupStart + GPUSignedDistanceFields_ScanGroupSize, buildInfo.numJobs) - 1u;
    let lastPrefix = ScratchJobCountersScan[JobScanIndex(cascadeIndex, lastJobIndex)];
    let lastTriangleCount = select(
        0u,
        LoadJobTriangleCount(cascadeIndex, lastJobIndex),
        atomicLoad(&ScratchJobCounters[JobCounterIndex(cascadeIndex, lastJobIndex)]) > 0u);
    return lastPrefix + lastTriangleCount;
}

fn LoadReferenceGroupSum(cascadeIndex: u32, groupIndex: u32) -> u32 {
    let lastVoxelIndex = min((groupIndex + 1u) * GPUSignedDistanceFields_ScanGroupSize, GPUSignedDistanceFields_CascadeVoxelCount) - 1u;
    return ScratchReferenceCounterScan[ReferenceCounterIndex(cascadeIndex, lastVoxelIndex)] +
        atomicLoad(&ScratchReferenceCounters[ReferenceCounterIndex(cascadeIndex, lastVoxelIndex)]);
}

fn LoadStampCount(cascadeIndex: u32, voxelIndex: u32) -> u32 {
    let refCount = atomicLoad(&ScratchReferenceCounters[ReferenceCounterIndex(cascadeIndex, voxelIndex)]);
    if (refCount == 0u) {
        return 0u;
    }

    let brickId = CascadeBrickMap[VoxelBase(cascadeIndex) + GPUSignedDistanceFieldsWrapFlatCoords(LoadCascadeInfo(cascadeIndex), voxelIndex)];
    if (!GPUSignedDistanceFieldsIsBrickValid(brickId)) {
        return 0u;
    }

    return (refCount + GPUSignedDistanceFields_EmitSdfRefsPerGroup - 1u) / GPUSignedDistanceFields_EmitSdfRefsPerGroup;
}

fn LoadStampGroupSum(cascadeIndex: u32, groupIndex: u32) -> u32 {
    let lastVoxelIndex = min((groupIndex + 1u) * GPUSignedDistanceFields_ScanGroupSize, GPUSignedDistanceFields_CascadeVoxelCount) - 1u;
    return ScratchStampScan[ReferenceCounterIndex(cascadeIndex, lastVoxelIndex)] + LoadStampCount(cascadeIndex, lastVoxelIndex);
}

fn AddBrickToCompressionList(cascadeIndex: u32, brickId: u32) {
    let offset = atomicAdd(&ScratchCounters[ScratchCounterBase(cascadeIndex) + GPUSignedDistanceFields_ScratchCounter_CompressionBricks], 1u);
    ScratchBrickCompressionList[BrickListIndex(cascadeIndex, offset)] = brickId;
}

fn AllocateBrick(cascadeIndex: u32) -> u32 {
    let brickIndex = atomicAdd(&ScratchCounters[ScratchCounterBase(cascadeIndex) + GPUSignedDistanceFields_ScratchCounter_NumBricksAllocated], 1u);
    if (brickIndex >= gUniforms.maxBricksPerBake) {
        return GPUSignedDistanceFields_InvalidId;
    }

    let freeListOffset = atomicAdd(&ContextCounters[GPUSignedDistanceFields_ContextCounter_BrickCount], 1u);
    if (freeListOffset >= atomicLoad(&ContextCounters[GPUSignedDistanceFields_ContextCounter_FreeBricks])) {
        return GPUSignedDistanceFields_InvalidId;
    }

    return ContextBrickFreeList[freeListOffset];
}

fn MapBrickToVoxel(cascadeIndex: u32, brickId: u32, voxelId: u32) {
    ContextBrickVoxelMap[brickId] = GPUSignedDistanceFieldsMakeVoxelId(cascadeIndex, voxelId);
}

fn BrickGetStorageOffset(cascadeIndex: u32, brickId: u32) -> u32 {
    return ScratchBrickStorageOffsets[BrickStorageOffsetIndex(cascadeIndex, brickId)];
}

fn AllocateStorage(cascadeIndex: u32, brickId: u32) -> u32 {
    let sizeBytes = GPUSignedDistanceFields_BrickCellCount * 4u;
    let base = ScratchCounterBase(cascadeIndex);
    let offset = atomicAdd(&ScratchCounters[base + GPUSignedDistanceFields_ScratchCounter_StorageOffset], sizeBytes);
    if (offset + sizeBytes > atomicLoad(&ScratchCounters[base + GPUSignedDistanceFields_ScratchCounter_StorageSize])) {
        ScratchBrickStorageOffsets[BrickStorageOffsetIndex(cascadeIndex, brickId)] = GPUSignedDistanceFields_InvalidId;
        return GPUSignedDistanceFields_InvalidId;
    }

    ScratchBrickStorageOffsets[BrickStorageOffsetIndex(cascadeIndex, brickId)] = offset;
    return offset;
}

fn AppendScratchClearBrick(cascadeIndex: u32, brickId: u32) {
    let offset = atomicAdd(&ScratchCounters[ScratchCounterBase(cascadeIndex) + GPUSignedDistanceFields_ScratchCounter_ClearBricks], 1u);
    ScratchBrickClearList[BrickListIndex(cascadeIndex, offset)] = brickId;
}

fn LoadScratchBrickDistance(cascadeIndex: u32, brickId: u32, coord: vec3<i32>) -> f32 {
    if (any(coord < vec3<i32>(0, 0, 0)) || any(coord >= vec3<i32>(8, 8, 8))) {
        return 1.0;
    }

    let offsetWords = BrickStorageWordBase(cascadeIndex) +
        (BrickGetStorageOffset(cascadeIndex, brickId) >> 2u) +
        GPUSignedDistanceFieldsFlatten3D(vec3<u32>(coord), GPUSignedDistanceFields_BrickResolution);
    return GPUSignedDistanceFieldsUnpackDistance(atomicLoad(&ScratchBrickStorage[offsetWords]));
}

fn ScratchBrickInterlockedMin(cascadeIndex: u32, brickId: u32, coord: vec3<i32>, value: f32) {
    if (any(coord < vec3<i32>(0, 0, 0)) || any(coord >= vec3<i32>(8, 8, 8))) {
        return;
    }

    let offsetWords = BrickStorageWordBase(cascadeIndex) +
        (BrickGetStorageOffset(cascadeIndex, brickId) >> 2u) +
        GPUSignedDistanceFieldsFlatten3D(vec3<u32>(coord), GPUSignedDistanceFields_BrickResolution);
    _ = atomicMin(&ScratchBrickStorage[offsetWords], GPUSignedDistanceFieldsPackDistance(value));
}

fn ClearScratchBrickDistance(cascadeIndex: u32, brickId: u32, coord: vec3<i32>) {
    if (any(coord < vec3<i32>(0, 0, 0)) || any(coord >= vec3<i32>(8, 8, 8))) {
        return;
    }

    let offsetWords = BrickStorageWordBase(cascadeIndex) +
        (BrickGetStorageOffset(cascadeIndex, brickId) >> 2u) +
        GPUSignedDistanceFieldsFlatten3D(vec3<u32>(coord), GPUSignedDistanceFields_BrickResolution);
    atomicStore(&ScratchBrickStorage[offsetWords], GPUSignedDistanceFieldsPackDistance(1.0));
}

fn StoreIndirectArgs(byteOffset: u32, x: u32, y: u32, z: u32) {
    let wordOffset = StageIndirectWordIndex(byteOffset);
    atomicStore(&IndirectArgs[wordOffset + 0u], x);
    atomicStore(&IndirectArgs[wordOffset + 1u], y);
    atomicStore(&IndirectArgs[wordOffset + 2u], z);
    atomicStore(&IndirectArgs[wordOffset + 3u], 0u);
}

fn StoreContextIndirectArgs(byteOffset: u32, x: u32, y: u32, z: u32) {
    let wordOffset = byteOffset / 4u;
    ContextIndirectArgs[wordOffset + 0u] = x;
    ContextIndirectArgs[wordOffset + 1u] = y;
    ContextIndirectArgs[wordOffset + 2u] = z;
    ContextIndirectArgs[wordOffset + 3u] = 0u;
}

fn InitializeIndirectArgs(cascadeIndex: u32, totalStampCount: u32) {
    let clearBrickCount = atomicLoad(&ScratchCounters[ScratchCounterBase(cascadeIndex) + GPUSignedDistanceFields_ScratchCounter_ClearBricks]);
    CascadeBuildInfos[cascadeIndex].clearBrickStorageDispatchX = clearBrickCount * 8u;
    CascadeBuildInfos[cascadeIndex].emitSdfDispatchX = totalStampCount;
    CascadeBuildInfos[cascadeIndex].compressBricksDispatchX =
        atomicLoad(&ScratchCounters[ScratchCounterBase(cascadeIndex) + GPUSignedDistanceFields_ScratchCounter_CompressionBricks]);

    let totalReferences = min(
        atomicLoad(&ScratchCounters[ScratchCounterBase(cascadeIndex) + GPUSignedDistanceFields_ScratchCounter_References]),
        atomicLoad(&ScratchCounters[ScratchCounterBase(cascadeIndex) + GPUSignedDistanceFields_ScratchCounter_MaxReferences]));
    let totalThreadCount = max(GPUSignedDistanceFields_CascadeVoxelCount, totalReferences);
    CascadeBuildInfos[cascadeIndex].compactReferencesDispatchX =
        (totalThreadCount + GPUSignedDistanceFields_CompactReferencesGroupSize - 1u) / GPUSignedDistanceFields_CompactReferencesGroupSize;

    _ = atomicMax(&IndirectArgs[StageIndirectWordIndex(GPUSignedDistanceFields_IndirectOffset_ClearBricks)], CascadeBuildInfos[cascadeIndex].clearBrickStorageDispatchX);
    _ = atomicMax(&IndirectArgs[StageIndirectWordIndex(GPUSignedDistanceFields_IndirectOffset_EmitSdf)], CascadeBuildInfos[cascadeIndex].emitSdfDispatchX);
    _ = atomicMax(&IndirectArgs[StageIndirectWordIndex(GPUSignedDistanceFields_IndirectOffset_CompressBricks)], CascadeBuildInfos[cascadeIndex].compressBricksDispatchX);
    _ = atomicMax(&IndirectArgs[StageIndirectWordIndex(GPUSignedDistanceFields_IndirectOffset_CompactReferences)], CascadeBuildInfos[cascadeIndex].compactReferencesDispatchX);
    let dispatchY = select(1u, gUniforms.cascadeCount, gUniforms.dispatchAllCascades != 0u);
    atomicStore(&IndirectArgs[StageIndirectWordIndex(GPUSignedDistanceFields_IndirectOffset_ClearBricks) + 1u], dispatchY);
    atomicStore(&IndirectArgs[StageIndirectWordIndex(GPUSignedDistanceFields_IndirectOffset_EmitSdf) + 1u], dispatchY);
    atomicStore(&IndirectArgs[StageIndirectWordIndex(GPUSignedDistanceFields_IndirectOffset_CompressBricks) + 1u], dispatchY);
    atomicStore(&IndirectArgs[StageIndirectWordIndex(GPUSignedDistanceFields_IndirectOffset_CompactReferences) + 1u], dispatchY);
    atomicStore(&IndirectArgs[StageIndirectWordIndex(GPUSignedDistanceFields_IndirectOffset_ClearBricks) + 2u], 1u);
    atomicStore(&IndirectArgs[StageIndirectWordIndex(GPUSignedDistanceFields_IndirectOffset_EmitSdf) + 2u], 1u);
    atomicStore(&IndirectArgs[StageIndirectWordIndex(GPUSignedDistanceFields_IndirectOffset_CompressBricks) + 2u], 1u);
    atomicStore(&IndirectArgs[StageIndirectWordIndex(GPUSignedDistanceFields_IndirectOffset_CompactReferences) + 2u], 1u);
}

fn InitializeJobIndirectArgs(cascadeIndex: u32, numTriangles: u32) {
    CascadeBuildInfos[cascadeIndex].voxelizeDispatchX =
        (numTriangles + GPUSignedDistanceFields_VoxelizerGroupSize - 1u) / GPUSignedDistanceFields_VoxelizerGroupSize;
    _ = atomicMax(
        &IndirectArgs[StageIndirectWordIndex(GPUSignedDistanceFields_IndirectOffset_Voxelize)],
        CascadeBuildInfos[cascadeIndex].voxelizeDispatchX);
    atomicStore(
        &IndirectArgs[StageIndirectWordIndex(GPUSignedDistanceFields_IndirectOffset_Voxelize) + 1u],
        select(1u, gUniforms.cascadeCount, gUniforms.dispatchAllCascades != 0u));
    atomicStore(&IndirectArgs[StageIndirectWordIndex(GPUSignedDistanceFields_IndirectOffset_Voxelize) + 2u], 1u);
}

fn LoadAtlasBrixelDistance(brickId: u32, localCoord: vec3<i32>) -> f32 {
    if (any(localCoord < vec3<i32>(0, 0, 0)) || any(localCoord >= vec3<i32>(8, 8, 8))) {
        return 1.0;
    }

    return textureLoad(SdfAtlas, vec3<i32>(GPUSignedDistanceFieldsAtlasCellCoord(brickId, vec3<u32>(localCoord), gUniforms.atlasBricksPerAxis))).x;
}

fn StoreAtlasBrixelDistance(brickId: u32, localCoord: vec3<u32>, distanceValue: f32) {
    textureStore(
        SdfAtlas,
        vec3<i32>(GPUSignedDistanceFieldsAtlasCellCoord(brickId, localCoord, gUniforms.atlasBricksPerAxis)),
        vec4<f32>(clamp(distanceValue, 0.0, 1.0), 0.0, 0.0, 0.0));
}

@compute @workgroup_size(1, 1, 1)
fn ClearCountersCS(@builtin(global_invocation_id) _dispatchThreadID: vec3<u32>) {
    for (var counterIndex = 0u; counterIndex < 6u; counterIndex = counterIndex + 1u) {
        atomicStore(&ContextCounters[counterIndex], 0u);
    }
}

@compute @workgroup_size(64, 1, 1)
fn CollectClearBricksCS(@builtin(global_invocation_id) dispatchThreadID: vec3<u32>) {
    let brickId = dispatchThreadID.x;
    if (brickId >= gUniforms.totalBricks) {
        return;
    }

    let voxelId = LoadBrickVoxelId(brickId);
    let eikonalCounter = ContextBrickEikonalCounters[brickId];

    if (gUniforms.doInitialization != 0u) {
        ContextBrickEikonalCounters[brickId] = 0u;
        MarkBrickFree(brickId);

        let freeOffset = atomicAdd(&ContextCounters[GPUSignedDistanceFields_ContextCounter_FreeBricks], 1u);
        ContextBrickFreeList[freeOffset] = brickId;

        let clearOffset = atomicAdd(&ContextCounters[GPUSignedDistanceFields_ContextCounter_ClearBricks], 1u);
        ContextBrickClearList[clearOffset] = brickId;
        return;
    }

    if (voxelId == GPUSignedDistanceFields_InvalidId) {
        let freeOffset = atomicAdd(&ContextCounters[GPUSignedDistanceFields_ContextCounter_FreeBricks], 1u);
        ContextBrickFreeList[freeOffset] = brickId;

        if (eikonalCounter > 0u) {
            let clearOffset = atomicAdd(&ContextCounters[GPUSignedDistanceFields_ContextCounter_ClearBricks], 1u);
            ContextBrickClearList[clearOffset] = brickId;
            ContextBrickEikonalCounters[brickId] = 0u;
        }
    }
}

@compute @workgroup_size(1, 1, 1)
fn PrepareClearBricksCS(@builtin(global_invocation_id) _dispatchThreadID: vec3<u32>) {
    StoreContextIndirectArgs(
        GPUSignedDistanceFields_IndirectOffset_ClearBricks,
        atomicLoad(&ContextCounters[GPUSignedDistanceFields_ContextCounter_ClearBricks]) * 8u,
        1u,
        1u);
}

@compute @workgroup_size(64, 1, 1)
fn ClearBrickEntryCS(@builtin(local_invocation_id) localInvocationID: vec3<u32>, @builtin(workgroup_id) groupId: vec3<u32>) {
    let brickOffset = groupId.x >> 3u;
    let stampId = groupId.x & 7u;
    let brickId = ContextBrickClearList[brickOffset];
    let localCoord = GPUSignedDistanceFieldsUnflatten3D(localInvocationID.x, 4u) + GPUSignedDistanceFieldsUnflatten3D(stampId, 2u) * 4u;
    StoreAtlasBrixelDistance(brickId, localCoord, 1.0);
}

@compute @workgroup_size(1, 1, 1)
fn ClearBuildCountersCS(@builtin(global_invocation_id) _dispatchThreadID: vec3<u32>) {
    let cascadeIndex = BuildCascadeIndex(_dispatchThreadID.y);
    let base = ScratchCounterBase(cascadeIndex);
    for (var counterIndex = 0u; counterIndex < 10u; counterIndex = counterIndex + 1u) {
        atomicStore(&ScratchCounters[base + counterIndex], 0u);
    }

    atomicStore(&ScratchCounters[base + GPUSignedDistanceFields_ScratchCounter_StorageSize], gUniforms.maxBricksPerBake * GPUSignedDistanceFields_BrickCellCount * 4u);
    atomicStore(&ScratchCounters[base + GPUSignedDistanceFields_ScratchCounter_MaxTriangles], gUniforms.maxTriangleSwapWords * 4u);
    atomicStore(&ScratchCounters[base + GPUSignedDistanceFields_ScratchCounter_MaxReferences], gUniforms.maxReferences);

    CascadeBuildInfos[cascadeIndex].voxelizeDispatchX = 0u;
    CascadeBuildInfos[cascadeIndex].compactReferencesDispatchX = 0u;
    CascadeBuildInfos[cascadeIndex].clearBrickStorageDispatchX = 0u;
    CascadeBuildInfos[cascadeIndex].emitSdfDispatchX = 0u;
    CascadeBuildInfos[cascadeIndex].compressBricksDispatchX = 0u;

    if (cascadeIndex == 0u) {
        for (var wordIndex = 0u; wordIndex < GPUSignedDistanceFields_IndirectOffset_Count / 4u; wordIndex = wordIndex + 1u) {
            atomicStore(&IndirectArgs[wordIndex], 0u);
        }

        let dispatchY = select(1u, gUniforms.cascadeCount, gUniforms.dispatchAllCascades != 0u);
        let beginCascade = select(gUniforms.cascadeIndex, 0u, gUniforms.dispatchAllCascades != 0u);
        let endCascade = select(gUniforms.cascadeIndex + 1u, gUniforms.cascadeCount, gUniforms.dispatchAllCascades != 0u);

        let seededOffsets = array<u32, 14>(
            GPUSignedDistanceFields_IndirectOffset_Voxelize,
            GPUSignedDistanceFields_IndirectOffset_CompactReferences,
            GPUSignedDistanceFields_IndirectOffset_ClearBricks,
            GPUSignedDistanceFields_IndirectOffset_EmitSdf,
            GPUSignedDistanceFields_IndirectOffset_CompressBricks,
            GPUSignedDistanceFields_IndirectOffset_ScrollCascade,
            GPUSignedDistanceFields_IndirectOffset_ClearRefCounters,
            GPUSignedDistanceFields_IndirectOffset_ClearJobCounter,
            GPUSignedDistanceFields_IndirectOffset_InvalidateJobAreas,
            GPUSignedDistanceFields_IndirectOffset_CoarseCulling,
            GPUSignedDistanceFields_IndirectOffset_ScanJobs,
            GPUSignedDistanceFields_IndirectOffset_FinalizeJobScan,
            GPUSignedDistanceFields_IndirectOffset_ScanReferences,
            GPUSignedDistanceFields_IndirectOffset_FinalizeReferenceScan);
        for (var i = 0u; i < 14u; i = i + 1u) {
            let wordOffset = StageIndirectWordIndex(seededOffsets[i]);
            atomicStore(&IndirectArgs[wordOffset + 1u], dispatchY);
            atomicStore(&IndirectArgs[wordOffset + 2u], 1u);
        }

        var maxJobCount = 0u;
        var maxJobVoxelCount = 0u;
        var anyCascadeScrolled = false;
        for (var seededCascadeIndex = beginCascade; seededCascadeIndex < endCascade; seededCascadeIndex = seededCascadeIndex + 1u) {
            let buildInfo = LoadBuildInfo(seededCascadeIndex);
            maxJobCount = max(maxJobCount, buildInfo.numJobs);
            maxJobVoxelCount = max(maxJobVoxelCount, buildInfo.numJobVoxels);

            let cascadeInfo = LoadCascadeInfo(seededCascadeIndex);
            anyCascadeScrolled = anyCascadeScrolled ||
                any(cascadeInfo.clipmapInvalidationOffset != vec3<i32>(0, 0, 0));
        }

        StoreIndirectArgs(
            GPUSignedDistanceFields_IndirectOffset_ScrollCascade,
            select(0u, (GPUSignedDistanceFields_CascadeVoxelCount + 64u - 1u) / 64u, anyCascadeScrolled),
            dispatchY,
            1u);
        StoreIndirectArgs(
            GPUSignedDistanceFields_IndirectOffset_ClearRefCounters,
            select(0u, (GPUSignedDistanceFields_CascadeVoxelCount + 64u - 1u) / 64u, maxJobVoxelCount > 0u),
            dispatchY,
            1u);
        StoreIndirectArgs(
            GPUSignedDistanceFields_IndirectOffset_ClearJobCounter,
            select(0u, (maxJobCount + 64u - 1u) / 64u, maxJobCount > 0u),
            dispatchY,
            1u);
        StoreIndirectArgs(
            GPUSignedDistanceFields_IndirectOffset_InvalidateJobAreas,
            select(0u, (maxJobVoxelCount + GPUSignedDistanceFields_VoxelizerGroupSize - 1u) / GPUSignedDistanceFields_VoxelizerGroupSize, maxJobVoxelCount > 0u),
            dispatchY,
            1u);
        StoreIndirectArgs(
            GPUSignedDistanceFields_IndirectOffset_CoarseCulling,
            select(0u, (maxJobVoxelCount + GPUSignedDistanceFields_VoxelizerGroupSize - 1u) / GPUSignedDistanceFields_VoxelizerGroupSize, maxJobVoxelCount > 0u),
            dispatchY,
            1u);
        StoreIndirectArgs(
            GPUSignedDistanceFields_IndirectOffset_ScanJobs,
            select(0u, (maxJobCount + GPUSignedDistanceFields_ScanGroupSize - 1u) / GPUSignedDistanceFields_ScanGroupSize, maxJobCount > 0u),
            dispatchY,
            1u);
        StoreIndirectArgs(
            GPUSignedDistanceFields_IndirectOffset_FinalizeJobScan,
            select(0u, 1u, maxJobCount > 0u),
            dispatchY,
            1u);
        StoreIndirectArgs(
            GPUSignedDistanceFields_IndirectOffset_ScanReferences,
            select(0u, (GPUSignedDistanceFields_CascadeVoxelCount + GPUSignedDistanceFields_ScanGroupSize - 1u) / GPUSignedDistanceFields_ScanGroupSize, maxJobVoxelCount > 0u),
            dispatchY,
            1u);
        StoreIndirectArgs(
            GPUSignedDistanceFields_IndirectOffset_FinalizeReferenceScan,
            select(0u, 1u, maxJobVoxelCount > 0u),
            dispatchY,
            1u);
    }
}

@compute @workgroup_size(64, 1, 1)
fn ResetCascadeCS(@builtin(global_invocation_id) dispatchThreadID: vec3<u32>) {
    let cascadeIndex = BuildCascadeIndex(dispatchThreadID.y);
    let tid = dispatchThreadID.x;
    if (tid >= GPUSignedDistanceFields_CascadeVoxelCount) {
        return;
    }

    let brickId = CascadeBrickMap[VoxelBase(cascadeIndex) + tid];
    if (GPUSignedDistanceFieldsIsBrickValid(brickId)) {
        MarkBrickFree(brickId);
    }
    CascadeBrickMap[VoxelBase(cascadeIndex) + tid] = GPUSignedDistanceFields_UninitializedId;
}

@compute @workgroup_size(64, 1, 1)
fn InitializeCascadeCS(@builtin(global_invocation_id) dispatchThreadID: vec3<u32>) {
    let cascadeIndex = BuildCascadeIndex(dispatchThreadID.y);
    let cascade = LoadCascadeInfo(cascadeIndex);
    let buildInfo = LoadBuildInfo(cascadeIndex);
    if (cascade.isInitialized != 0u || buildInfo.numJobVoxels != 0u) {
        return;
    }
    let tid = dispatchThreadID.x;
    if (tid >= GPUSignedDistanceFields_CascadeVoxelCount) {
        return;
    }

    let index = VoxelBase(cascadeIndex) + tid;
    if (CascadeBrickMap[index] == GPUSignedDistanceFields_UninitializedId) {
        CascadeBrickMap[index] = GPUSignedDistanceFields_InvalidId;
    }
}

@compute @workgroup_size(64, 1, 1)
fn MarkCascadeUninitializedCS(@builtin(global_invocation_id) dispatchThreadID: vec3<u32>) {
    let cascadeIndex = BuildCascadeIndex(dispatchThreadID.y);
    if (LoadCascadeInfo(cascadeIndex).isInitialized != 0u) {
        return;
    }
    let tid = dispatchThreadID.x;
    if (tid < GPUSignedDistanceFields_CascadeVoxelCount) {
        CascadeBrickMap[VoxelBase(cascadeIndex) + tid] = GPUSignedDistanceFields_UninitializedId;
    }
}

@compute @workgroup_size(64, 1, 1)
fn FreeCascadeCS(@builtin(global_invocation_id) dispatchThreadID: vec3<u32>) {
    let cascadeIndex = BuildCascadeIndex(dispatchThreadID.y);
    let tid = dispatchThreadID.x;
    if (tid >= GPUSignedDistanceFields_CascadeVoxelCount) {
        return;
    }

    let brickId = CascadeBrickMap[VoxelBase(cascadeIndex) + tid];
    if (GPUSignedDistanceFieldsIsBrickValid(brickId)) {
        MarkBrickFree(brickId);
    }
    CascadeBrickMap[VoxelBase(cascadeIndex) + tid] = GPUSignedDistanceFields_UninitializedId;
}

@compute @workgroup_size(64, 1, 1)
fn ScrollCascadeCS(@builtin(global_invocation_id) dispatchThreadID: vec3<u32>) {
    let cascadeIndex = BuildCascadeIndex(dispatchThreadID.y);
    let cascade = LoadCascadeInfo(cascadeIndex);
    let tid = dispatchThreadID.x;
    if (tid >= GPUSignedDistanceFields_CascadeVoxelCount) {
        return;
    }

    let voxelCoord = vec3<i32>(GPUSignedDistanceFieldsUnflatten3D(tid, GPUSignedDistanceFields_CascadeResolution));
    if (any(voxelCoord < -cascade.clipmapInvalidationOffset) ||
        any(voxelCoord >= vec3<i32>(i32(GPUSignedDistanceFields_CascadeResolution)) - cascade.clipmapInvalidationOffset)) {
        let wrappedVoxelIndex = GPUSignedDistanceFieldsWrapFlatCoords(cascade, tid);
        let brickMapIndex = VoxelBase(cascadeIndex) + wrappedVoxelIndex;
        let brickId = CascadeBrickMap[brickMapIndex];
        if (GPUSignedDistanceFieldsIsBrickValid(brickId)) {
            MarkBrickFree(brickId);
        }
        CascadeBrickMap[brickMapIndex] = GPUSignedDistanceFields_UninitializedId;
    } else {
        let wrappedVoxelIndex = GPUSignedDistanceFieldsWrapFlatCoords(cascade, tid);
        let brickMapIndex = VoxelBase(cascadeIndex) + wrappedVoxelIndex;
        let brickId = CascadeBrickMap[brickMapIndex];
        if (GPUSignedDistanceFieldsIsBrickValid(brickId)) {
            // After the clipmap offset changes, the physical slot is still correct,
            // but the brick's logical voxel id must be refreshed to the new clipmap-space index.
            MapBrickToVoxel(cascadeIndex, brickId, tid);
        }
    }
}

@compute @workgroup_size(64, 1, 1)
fn ClearRefCountersCS(@builtin(global_invocation_id) dispatchThreadID: vec3<u32>) {
    let cascadeIndex = BuildCascadeIndex(dispatchThreadID.y);
    let tid = dispatchThreadID.x;
    if (tid >= GPUSignedDistanceFields_CascadeVoxelCount) {
        return;
    }

    ClearRefCounter(cascadeIndex, tid);
    ScratchReferenceCounterScan[ReferenceCounterIndex(cascadeIndex, tid)] = 0u;
    ScratchStampScan[ReferenceCounterIndex(cascadeIndex, tid)] = 0u;
}

@compute @workgroup_size(64, 1, 1)
fn ClearJobCounterCS(@builtin(global_invocation_id) dispatchThreadID: vec3<u32>) {
    let cascadeIndex = BuildCascadeIndex(dispatchThreadID.y);
    let tid = dispatchThreadID.x;
    if (tid < LoadBuildInfo(cascadeIndex).numJobs) {
        atomicStore(&ScratchJobCounters[JobCounterIndex(cascadeIndex, tid)], 0u);
    }
}

@compute @workgroup_size(256, 1, 1)
fn InvalidateJobAreasCS(@builtin(local_invocation_id) localInvocationID: vec3<u32>, @builtin(workgroup_id) groupId: vec3<u32>) {
    let cascadeIndex = BuildCascadeIndex(groupId.y);
    let threadSubvoxelOffset = groupId.x * GPUSignedDistanceFields_VoxelizerGroupSize + localInvocationID.x;
    let jobIndex = JobLowerBoundBySubvoxel(cascadeIndex, threadSubvoxelOffset);
    if (jobIndex >= LoadBuildInfo(cascadeIndex).numJobs) {
        return;
    }

    let job = Jobs[JobBase(cascadeIndex) + jobIndex];
    if (!IsJobInvalidating(job)) {
        return;
    }

    let dim = job.aabbMax - job.aabbMin;
    let subvoxelId = threadSubvoxelOffset - JobSubvoxelOffsets[JobBase(cascadeIndex) + jobIndex];
    if (subvoxelId >= dim.x * dim.y * dim.z) {
        return;
    }

    let subvoxelCoord = GPUSignedDistanceFieldsUnflatten(subvoxelId, dim);
    let globalVoxelCoord = subvoxelCoord + job.aabbMin;
    let voxelIndex = GPUSignedDistanceFieldsFlatten3D(globalVoxelCoord, GPUSignedDistanceFields_CascadeResolution);
    let wrappedVoxelIndex = GPUSignedDistanceFieldsWrapFlatCoords(LoadCascadeInfo(cascadeIndex), voxelIndex);
    let brickMapIndex = VoxelBase(cascadeIndex) + wrappedVoxelIndex;
    let brickId = CascadeBrickMap[brickMapIndex];
    if (brickId != GPUSignedDistanceFields_UninitializedId) {
        if (GPUSignedDistanceFieldsIsBrickValid(brickId)) {
            MarkBrickFree(brickId);
        }
        CascadeBrickMap[brickMapIndex] = GPUSignedDistanceFields_UninitializedId;
    }
}

@compute @workgroup_size(256, 1, 1)
fn CoarseCullingCS(@builtin(local_invocation_id) localInvocationID: vec3<u32>, @builtin(workgroup_id) groupId: vec3<u32>) {
    let cascadeIndex = BuildCascadeIndex(groupId.y);
    let threadSubvoxelOffset = groupId.x * GPUSignedDistanceFields_VoxelizerGroupSize + localInvocationID.x;
    let jobIndex = JobLowerBoundBySubvoxel(cascadeIndex, threadSubvoxelOffset);
    if (jobIndex >= LoadBuildInfo(cascadeIndex).numJobs) {
        return;
    }

    let job = Jobs[JobBase(cascadeIndex) + jobIndex];
    if (IsJobInvalidating(job)) {
        return;
    }

    let dim = job.aabbMax - job.aabbMin;
    let subvoxelId = threadSubvoxelOffset - JobSubvoxelOffsets[JobBase(cascadeIndex) + jobIndex];
    if (subvoxelId >= dim.x * dim.y * dim.z) {
        return;
    }

    let subvoxelCoord = GPUSignedDistanceFieldsUnflatten(subvoxelId, dim);
    let globalVoxelCoord = subvoxelCoord + job.aabbMin;
    let voxelIndex = GPUSignedDistanceFieldsFlatten3D(globalVoxelCoord, GPUSignedDistanceFields_CascadeResolution);
    if (IsBuildable(cascadeIndex, voxelIndex)) {
        _ = atomicAdd(&ScratchJobCounters[JobCounterIndex(cascadeIndex, jobIndex)], 1u);
    }
}

@compute @workgroup_size(256, 1, 1)
fn ScanJobsCS(@builtin(global_invocation_id) dispatchThreadID: vec3<u32>, @builtin(local_invocation_id) localInvocationID: vec3<u32>, @builtin(workgroup_id) groupId: vec3<u32>) {
    let cascadeIndex = BuildCascadeIndex(groupId.y);
    let jobIndex = dispatchThreadID.x;
    let gtid = localInvocationID.x;
    let buildInfo = LoadBuildInfo(cascadeIndex);
    let isTouched = jobIndex < buildInfo.numJobs && atomicLoad(&ScratchJobCounters[JobCounterIndex(cascadeIndex, jobIndex)]) > 0u;
    let triangleCount = select(0u, LoadJobTriangleCount(cascadeIndex, jobIndex), isTouched);

    gsScanBuffer[gtid] = triangleCount;
    workgroupBarrier();
    _ = GroupScanExclusiveAdd(gtid, GPUSignedDistanceFields_ScanGroupSize);
    if (jobIndex < buildInfo.numJobs) {
        ScratchJobCountersScan[JobScanIndex(cascadeIndex, jobIndex)] = gsScanBuffer[gtid];
    }

    if (gtid == 0u) {
        ScratchJobGlobalCountersScan[JobGlobalScanIndex(cascadeIndex, groupId.x)] = 0u;
    }
}

@compute @workgroup_size(256, 1, 1)
fn FinalizeJobScanCS(@builtin(local_invocation_id) localInvocationID: vec3<u32>, @builtin(workgroup_id) groupId: vec3<u32>) {
    let cascadeIndex = BuildCascadeIndex(groupId.y);
    let gtid = localInvocationID.x;
    let totalGroupCount = (LoadBuildInfo(cascadeIndex).numJobs + GPUSignedDistanceFields_ScanGroupSize - 1u) / GPUSignedDistanceFields_ScanGroupSize;

    if (gtid == 0u) {
        atomicStore(&gsScanCarry0, 0u);
    }
    workgroupBarrier();

    for (var cursor = 0u; cursor < totalGroupCount; cursor = cursor + GPUSignedDistanceFields_ScanGroupSize) {
        let groupIndex = cursor + gtid;
        gsScanBuffer[gtid] = select(0u, LoadJobGroupTriangleSum(cascadeIndex, groupIndex), groupIndex < totalGroupCount);
        workgroupBarrier();
        let chunkSum = GroupScanExclusiveAdd(gtid, GPUSignedDistanceFields_ScanGroupSize);
        if (groupIndex < totalGroupCount) {
            ScratchJobGlobalCountersScan[JobGlobalScanIndex(cascadeIndex, groupIndex)] = gsScanBuffer[gtid] + atomicLoad(&gsScanCarry0);
        }
        workgroupBarrier();
        if (gtid == 0u) {
            _ = atomicAdd(&gsScanCarry0, chunkSum);
        }
        workgroupBarrier();
    }

    if (gtid == 0u) {
        InitializeJobIndirectArgs(cascadeIndex, atomicLoad(&gsScanCarry0));
    }
}

@compute @workgroup_size(256, 1, 1)
fn VoxelizeCS(@builtin(local_invocation_id) localInvocationID: vec3<u32>, @builtin(workgroup_id) groupId: vec3<u32>) {
    let cascadeIndex = BuildCascadeIndex(groupId.y);
    let voxelizeDispatchX = LoadBuildInfo(cascadeIndex).voxelizeDispatchX;
    if (groupId.x >= voxelizeDispatchX) {
        return;
    }
    let gtid = localInvocationID.x;
    if (gtid == 0u) {
        atomicStore(&gsVoxelizerItemCounter, 0u);
        atomicStore(&gsVoxelizerRefCounter, 0u);
        atomicStore(&gsVoxelizerRefOffset, 0u);
        atomicStore(&gsTriangleOffsetGlobal, 0u);
        atomicStore(&gsTriangleOffset, 0u);
        atomicStore(&gsVoxelizerHasSpace, 0u);
    }
    workgroupBarrier();

    let threadTriangleOffset = groupId.x * GPUSignedDistanceFields_VoxelizerGroupSize + gtid;
    let jobIndex = JobLowerBound(cascadeIndex, threadTriangleOffset);
    let buildInfo = LoadBuildInfo(cascadeIndex);
    let isTouched = jobIndex < buildInfo.numJobs && atomicLoad(&ScratchJobCounters[JobCounterIndex(cascadeIndex, jobIndex)]) > 0u;
    let triangleIndex = threadTriangleOffset - LoadJobTriangleCountScan(cascadeIndex, jobIndex);

    var tri : TriangleData;
    var bounds : TriangleVoxelBounds;
    var collides = false;
    if (isTouched && triangleIndex < LoadJobTriangleCount(cascadeIndex, jobIndex)) {
        tri = FetchTriangle(cascadeIndex, triangleIndex, jobIndex);
        bounds = GetTriangleBounds(cascadeIndex, jobIndex, triangleIndex);
        collides = all(bounds.voxelMax > bounds.voxelMin);
    }

    var itemOffset = 0u;
    if (collides) {
        itemOffset = atomicAdd(&gsVoxelizerItemCounter, 1u);
        var item : CRItem;
        item.bounds_min = bounds.voxelMin;
        item.bounds_max = bounds.voxelMax;
        StoreCRItem(itemOffset, item);

        let dim = bounds.voxelMax - bounds.voxelMin;
        let numRefs = dim.x * dim.y * dim.z;
        _ = atomicAdd(&gsVoxelizerRefCounter, numRefs);
        gsVoxelizerItemsRefCount[itemOffset] = numRefs;
        gsVoxelizerItemsTriangleIdSwapOffsets[itemOffset] = 0u;
    }

    workgroupBarrier();
    if (atomicLoad(&gsVoxelizerItemCounter) == 0u) {
        return;
    }

    let maxStorage = atomicLoad(&ScratchCounters[ScratchCounterBase(cascadeIndex) + GPUSignedDistanceFields_ScratchCounter_MaxTriangles]);
    let triBounds = bounds.boundMax - bounds.boundMin;
    let aabbMaxDim = max(triBounds.x, max(triBounds.y, triBounds.z));
    let voxelSizeRatio = aabbMaxDim / LoadCascadeInfo(cascadeIndex).voxelSize;
    let smallTriangle = voxelSizeRatio < 1.0e-1;
    let triangleSizeBytes = select(36u, 24u, smallTriangle);
    let hitCount = 1u;

    var localTriangleSwapOffset = 0u;
    if (collides && hitCount != 0u) {
        localTriangleSwapOffset = atomicAdd(&gsTriangleOffset, triangleSizeBytes);
    }

    workgroupBarrier();
    if (gtid == 0u) {
        let globalOffset = atomicAdd(&ScratchCounters[ScratchCounterBase(cascadeIndex) + GPUSignedDistanceFields_ScratchCounter_Triangles], atomicLoad(&gsTriangleOffset));
        atomicStore(&gsTriangleOffsetGlobal, globalOffset);
        atomicStore(&gsVoxelizerHasSpace, select(0u, 1u, globalOffset + atomicLoad(&gsTriangleOffset) <= maxStorage));
    }
    workgroupBarrier();

    if (collides && atomicLoad(&gsVoxelizerHasSpace) != 0u) {
        if (hitCount != 0u) {
            var triangleIdSwapOffset = localTriangleSwapOffset + atomicLoad(&gsTriangleOffsetGlobal);
            if (smallTriangle) {
                StoreTriangleCenter(cascadeIndex, triangleIdSwapOffset, tri);
                triangleIdSwapOffset = triangleIdSwapOffset | GPUSignedDistanceFields_TriangleSmallFlag;
            } else {
                StoreTrianglePartial(cascadeIndex, triangleIdSwapOffset, tri);
            }
            gsVoxelizerItemsTriangleIdSwapOffsets[itemOffset] = triangleIdSwapOffset;
        } else {
            gsVoxelizerItemsTriangleIdSwapOffsets[itemOffset] = 0xffffffffu;
        }
    }

    workgroupBarrier();

    var itemId = 0u;
    var refScan = 0u;
    for (var refId = gtid; refId < atomicLoad(&gsVoxelizerRefCounter); refId = refId + GPUSignedDistanceFields_VoxelizerGroupSize) {
        while (refId >= gsVoxelizerItemsRefCount[itemId] + refScan) {
            refScan = refScan + gsVoxelizerItemsRefCount[itemId];
            itemId = itemId + 1u;
        }

        if (gsVoxelizerItemsTriangleIdSwapOffsets[itemId] == 0xffffffffu) {
            continue;
        }

        let localRefId = refId - refScan;
        let item = LoadCRItem(itemId);
        let dim = item.bounds_max - item.bounds_min;
        let localVoxelCoord = GPUSignedDistanceFieldsUnflatten(localRefId, dim);
        let globalVoxelCoord = localVoxelCoord + item.bounds_min;
        let voxelIndex = GPUSignedDistanceFieldsFlatten3D(globalVoxelCoord, GPUSignedDistanceFields_CascadeResolution);

        if (atomicLoad(&gsVoxelizerHasSpace) == 0u) {
            MarkFailed(cascadeIndex, voxelIndex);
        } else {
            AddReferenceOrMarkVoxelFailed(cascadeIndex, voxelIndex, gsVoxelizerItemsTriangleIdSwapOffsets[itemId]);
        }
    }
}

@compute @workgroup_size(256, 1, 1)
fn ScanReferencesCS(@builtin(global_invocation_id) dispatchThreadID: vec3<u32>, @builtin(local_invocation_id) localInvocationID: vec3<u32>, @builtin(workgroup_id) groupId: vec3<u32>) {
    let cascadeIndex = BuildCascadeIndex(groupId.y);
    let voxelIndex = dispatchThreadID.x;
    let gtid = localInvocationID.x;
    if (voxelIndex >= GPUSignedDistanceFields_CascadeVoxelCount) {
        return;
    }

    var refCount = atomicLoad(&ScratchReferenceCounters[ReferenceCounterIndex(cascadeIndex, voxelIndex)]);
    let failedAtVoxelizer = ScratchVoxelAllocationFailCounters[ReferenceCounterIndex(cascadeIndex, voxelIndex)];
    if (failedAtVoxelizer != 0u) {
        refCount = 0u;
        ClearRefCounter(cascadeIndex, voxelIndex);
    }

    let wrappedVoxelIndex = GPUSignedDistanceFieldsWrapFlatCoords(LoadCascadeInfo(cascadeIndex), voxelIndex);
    let brickMapIndex = VoxelBase(cascadeIndex) + wrappedVoxelIndex;
    var brickId = CascadeBrickMap[brickMapIndex];
    if (refCount > 0u) {
        if (brickId == GPUSignedDistanceFields_UninitializedId) {
            brickId = AllocateBrick(cascadeIndex);
            if (GPUSignedDistanceFieldsIsInvalidId(brickId)) {
                refCount = 0u;
                ClearRefCounter(cascadeIndex, voxelIndex);
                CascadeBrickMap[brickMapIndex] = GPUSignedDistanceFields_UninitializedId;
            } else {
                let storageOffset = AllocateStorage(cascadeIndex, brickId);
                if (storageOffset == GPUSignedDistanceFields_InvalidId) {
                    refCount = 0u;
                    ClearRefCounter(cascadeIndex, voxelIndex);
                    MarkBrickFree(brickId);
                    CascadeBrickMap[brickMapIndex] = GPUSignedDistanceFields_UninitializedId;
                    brickId = GPUSignedDistanceFields_InvalidId;
                } else {
                    AppendScratchClearBrick(cascadeIndex, brickId);
                    AddBrickToCompressionList(cascadeIndex, brickId);
                    CascadeBrickMap[brickMapIndex] = brickId;
                }
            }
        } else {
            refCount = 0u;
            ClearRefCounter(cascadeIndex, voxelIndex);
        }
    } else if (failedAtVoxelizer == 0u && brickId == GPUSignedDistanceFields_UninitializedId) {
        brickId = GPUSignedDistanceFields_InvalidId;
        CascadeBrickMap[brickMapIndex] = GPUSignedDistanceFields_InvalidId;
        ClearRefCounter(cascadeIndex, voxelIndex);
    }

    if (GPUSignedDistanceFieldsIsBrickValid(brickId)) {
        MapBrickToVoxel(cascadeIndex, brickId, voxelIndex);
    }

    gsScanBuffer[gtid] = refCount;
    workgroupBarrier();
    _ = GroupScanExclusiveAdd(gtid, GPUSignedDistanceFields_ScanGroupSize);
    ScratchReferenceCounterScan[ReferenceCounterIndex(cascadeIndex, voxelIndex)] = gsScanBuffer[gtid];
    if (gtid == 0u) {
        ScratchReferenceGlobalScan[ReferenceGlobalScanIndex(cascadeIndex, groupId.x)] = 0u;
    }

    var stampCount = 0u;
    if (refCount > 0u && GPUSignedDistanceFieldsIsBrickValid(brickId)) {
        stampCount = (refCount + GPUSignedDistanceFields_EmitSdfRefsPerGroup - 1u) / GPUSignedDistanceFields_EmitSdfRefsPerGroup;
    }

    gsScanBuffer[gtid] = stampCount;
    workgroupBarrier();
    _ = GroupScanExclusiveAdd(gtid, GPUSignedDistanceFields_ScanGroupSize);
    ScratchStampScan[ReferenceCounterIndex(cascadeIndex, voxelIndex)] = gsScanBuffer[gtid];
    if (gtid == 0u) {
        ScratchStampGlobalScan[StampGlobalScanIndex(cascadeIndex, groupId.x)] = 0u;
    }
}

@compute @workgroup_size(256, 1, 1)
fn FinalizeReferenceScanCS(@builtin(local_invocation_id) localInvocationID: vec3<u32>, @builtin(workgroup_id) groupId: vec3<u32>) {
    let cascadeIndex = BuildCascadeIndex(groupId.y);
    let gtid = localInvocationID.x;
    let totalGroupCount = GPUSignedDistanceFields_CascadeVoxelCount / GPUSignedDistanceFields_ScanGroupSize;

    if (gtid == 0u) {
        atomicStore(&gsScanCarry0, 0u);
        atomicStore(&gsScanCarry1, 0u);
    }
    workgroupBarrier();

    for (var cursor = 0u; cursor < totalGroupCount; cursor = cursor + GPUSignedDistanceFields_ScanGroupSize) {
        let groupIndex = cursor + gtid;
        gsScanBuffer[gtid] = select(0u, LoadReferenceGroupSum(cascadeIndex, groupIndex), groupIndex < totalGroupCount);
        workgroupBarrier();
        let chunkSum = GroupScanExclusiveAdd(gtid, GPUSignedDistanceFields_ScanGroupSize);
        if (groupIndex < totalGroupCount) {
            ScratchReferenceGlobalScan[ReferenceGlobalScanIndex(cascadeIndex, groupIndex)] = gsScanBuffer[gtid] + atomicLoad(&gsScanCarry0);
        }
        workgroupBarrier();
        if (gtid == 0u) {
            _ = atomicAdd(&gsScanCarry0, chunkSum);
        }
        workgroupBarrier();
    }

    for (var cursor = 0u; cursor < totalGroupCount; cursor = cursor + GPUSignedDistanceFields_ScanGroupSize) {
        let groupIndex = cursor + gtid;
        gsScanBuffer[gtid] = select(0u, LoadStampGroupSum(cascadeIndex, groupIndex), groupIndex < totalGroupCount);
        workgroupBarrier();
        let chunkSum = GroupScanExclusiveAdd(gtid, GPUSignedDistanceFields_ScanGroupSize);
        if (groupIndex < totalGroupCount) {
            ScratchStampGlobalScan[StampGlobalScanIndex(cascadeIndex, groupIndex)] = gsScanBuffer[gtid] + atomicLoad(&gsScanCarry1);
        }
        workgroupBarrier();
        if (gtid == 0u) {
            _ = atomicAdd(&gsScanCarry1, chunkSum);
        }
        workgroupBarrier();
    }

    if (gtid == 0u) {
        InitializeIndirectArgs(cascadeIndex, atomicLoad(&gsScanCarry1));
    }
}

@compute @workgroup_size(64, 1, 1)
fn CompactReferencesCS(@builtin(global_invocation_id) dispatchThreadID: vec3<u32>) {
    let cascadeIndex = BuildCascadeIndex(dispatchThreadID.y);
    let compactReferencesDispatchX = LoadBuildInfo(cascadeIndex).compactReferencesDispatchX;
    let dispatchGroupIndex = dispatchThreadID.x / GPUSignedDistanceFields_CompactReferencesGroupSize;
    if (dispatchGroupIndex >= compactReferencesDispatchX) {
        return;
    }
    let tid = dispatchThreadID.x;
    let totalReferences = min(
        atomicLoad(&ScratchCounters[ScratchCounterBase(cascadeIndex) + GPUSignedDistanceFields_ScratchCounter_References]),
        atomicLoad(&ScratchCounters[ScratchCounterBase(cascadeIndex) + GPUSignedDistanceFields_ScratchCounter_MaxReferences]));

    if (tid < totalReferences) {
        let reference = ScratchRawReferences[RawReferenceIndex(cascadeIndex, tid)];
        let voxelIndex = reference.voxelIndex;
        let refCount = atomicLoad(&ScratchReferenceCounters[ReferenceCounterIndex(cascadeIndex, voxelIndex)]);
        if (refCount > 0u) {
            let offset = GetReferenceOffset(cascadeIndex, voxelIndex) + reference.localReferenceIndex;
            ScratchCompactedReferences[CompactedReferenceIndex(cascadeIndex, offset)] = reference.triangleId;
        }
    }

    if (tid < GPUSignedDistanceFields_CascadeVoxelCount) {
        let groupScanId = tid / GPUSignedDistanceFields_ScanGroupSize;
        ScratchStampScan[ReferenceCounterIndex(cascadeIndex, tid)] =
            ScratchStampGlobalScan[StampGlobalScanIndex(cascadeIndex, groupScanId)] +
            ScratchStampScan[ReferenceCounterIndex(cascadeIndex, tid)];
    }
}

@compute @workgroup_size(64, 1, 1)
fn ClearBrickStorageCS(@builtin(local_invocation_id) localInvocationID: vec3<u32>, @builtin(workgroup_id) groupId: vec3<u32>) {
    let cascadeIndex = BuildCascadeIndex(groupId.y);
    let clearBrickDispatchX = LoadBuildInfo(cascadeIndex).clearBrickStorageDispatchX;
    if (groupId.x >= clearBrickDispatchX) {
        return;
    }
    let brickOffset = groupId.x >> 3u;
    let stampId = groupId.x & 7u;
    let brickId = ScratchBrickClearList[BrickListIndex(cascadeIndex, brickOffset)];
    let surfaceTexelIndex = stampId * 64u + localInvocationID.x;
    if (surfaceTexelIndex < GPUSignedDistanceFields_SurfaceCacheFaceCount * SurfaceCachePageTexelCount()) {
        SurfaceCacheClearMaterialReferenceTexel(brickId, surfaceTexelIndex);
    }
    let localCoord = GPUSignedDistanceFieldsUnflatten3D(localInvocationID.x, 4u) + GPUSignedDistanceFieldsUnflatten3D(stampId, 2u) * 4u;
    ClearScratchBrickDistance(cascadeIndex, brickId, vec3<i32>(localCoord));
}

@compute @workgroup_size(32, 1, 1)
fn EmitSdfCS(@builtin(local_invocation_id) localInvocationID: vec3<u32>, @builtin(workgroup_id) groupId: vec3<u32>) {
    let cascadeIndex = BuildCascadeIndex(groupId.y);
    if (groupId.x >= LoadBuildInfo(cascadeIndex).emitSdfDispatchX) {
        return;
    }
    let gtid = localInvocationID.x;
    let globalStampId = groupId.x;
    let voxelIndex = StampLowerBound(cascadeIndex, globalStampId);
    let cascade = LoadCascadeInfo(cascadeIndex);
    let brickId = CascadeBrickMap[VoxelBase(cascadeIndex) + GPUSignedDistanceFieldsWrapFlatCoords(cascade, voxelIndex)];
    let refCount = atomicLoad(&ScratchReferenceCounters[ReferenceCounterIndex(cascadeIndex, voxelIndex)]);
    if (!GPUSignedDistanceFieldsIsBrickValid(brickId) || refCount == 0u) {
        return;
    }

    let refBatchCount = (refCount + GPUSignedDistanceFields_EmitSdfRefsPerGroup - 1u) / GPUSignedDistanceFields_EmitSdfRefsPerGroup;
    let globalStampOffset = ScratchStampScan[ReferenceCounterIndex(cascadeIndex, voxelIndex)];
    let voxelStampId = globalStampId - globalStampOffset;
    let refBatchId = voxelStampId % refBatchCount;
    let voxelRefOffset = GetReferenceOffset(cascadeIndex, voxelIndex);
    let refBatchOffset = refBatchId * GPUSignedDistanceFields_EmitSdfRefsPerGroup;
    let startRefId = voxelRefOffset + refBatchOffset;
    let endRefId = voxelRefOffset + min(refBatchOffset + GPUSignedDistanceFields_EmitSdfRefsPerGroup, refCount);

    let stampMin = vec3<u32>(0u, 0u, 0u);
    let stampMax = stampMin + vec3<u32>(8u, 8u, 8u);
    let brickWidth = 8.0;
    let voxelCoord = GPUSignedDistanceFieldsUnflatten3D(voxelIndex, GPUSignedDistanceFields_CascadeResolution);
    let brixelSize = cascade.voxelSize / (brickWidth - 1.0);
    let halfBrixelSize = brixelSize * 0.5;
    let brickMin = vec3<f32>(voxelCoord) * cascade.voxelSize - vec3<f32>(halfBrixelSize);

    for (var refId = startRefId + gtid; refId < endRefId; refId = refId + GPUSignedDistanceFields_EmitSdfGroupSize) {
        let triangleId = ScratchCompactedReferences[CompactedReferenceIndex(cascadeIndex, refId)];
        if (GPUSignedDistanceFieldsTriangleIsSmall(triangleId)) {
            let center = LoadTriangleCenter(cascadeIndex, GPUSignedDistanceFieldsTriangleIdGetOffset(triangleId));
            let coord = (center.center - brickMin) / brixelSize;
            let voxel = clamp(floor(coord), vec3<f32>(0.0, 0.0, 0.0), vec3<f32>(7.0, 7.0, 7.0));
            let p = voxel + vec3<f32>(0.5, 0.5, 0.5);
            let dist = GPUSignedDistanceFieldsDot2Vec3(p - coord) * 0.25 * 0.25;
            ScratchBrickInterlockedMin(cascadeIndex, brickId, vec3<i32>(voxel), clamp(dist, 0.0, 1.0));
            let centerSurface = SurfaceCacheFetchTriangleCenterSurface(cascadeIndex, center);
            SurfaceCacheAccumulateTriangleMaterial(
                cascadeIndex,
                brickId,
                centerSurface,
                center.jobIndex,
                center.triangleIndex,
                center.center,
                center.center,
                center.center,
                center.center,
                clamp(dist, 0.0, 1.0),
                brixelSize,
                vec3<f32>(0.0, 0.0, 1.0),
                clamp((p - vec3<f32>(0.5, 0.5, 0.5)) / 7.0, vec3<f32>(0.0), vec3<f32>(0.999999)));
            continue;
        }

        let tri = LoadTrianglePartial(cascadeIndex, GPUSignedDistanceFieldsTriangleIdGetOffset(triangleId));
        let triangleVertex0 = (tri.wp0 - brickMin) / brixelSize;
        let triangleVertex1 = (tri.wp1 - brickMin) / brixelSize;
        let triangleVertex2 = (tri.wp2 - brickMin) / brixelSize;
        let triangleOffset = 0.0;
        let triangleMin = min(triangleVertex0, min(triangleVertex1, triangleVertex2));
        let triangleMax = max(triangleVertex0, max(triangleVertex1, triangleVertex2));

        var triangleAabbMin : vec3<f32>;
        triangleAabbMin.x = floor(select(triangleMin.x, triangleMin.x - 1.0, triangleMin.x < 0.0)) - triangleOffset;
        triangleAabbMin.y = floor(select(triangleMin.y, triangleMin.y - 1.0, triangleMin.y < 0.0)) - triangleOffset;
        triangleAabbMin.z = floor(select(triangleMin.z, triangleMin.z - 1.0, triangleMin.z < 0.0)) - triangleOffset;

        var triangleAabbMax : vec3<f32>;
        triangleAabbMax.x = floor(select(triangleMax.x, triangleMax.x - 1.0, triangleMax.x < 0.0)) + (1.0 + triangleOffset);
        triangleAabbMax.y = floor(select(triangleMax.y, triangleMax.y - 1.0, triangleMax.y < 0.0)) + (1.0 + triangleOffset);
        triangleAabbMax.z = floor(select(triangleMax.z, triangleMax.z - 1.0, triangleMax.z < 0.0)) + (1.0 + triangleOffset);

        triangleAabbMin = max(triangleAabbMin, vec3<f32>(stampMin));
        triangleAabbMax = min(triangleAabbMax, vec3<f32>(stampMax));
        if (all(triangleAabbMin == triangleAabbMax)) {
            continue;
        }

        let a = triangleVertex0;
        let b = triangleVertex1;
        let c = triangleVertex2;
        let ba = triangleVertex1 - triangleVertex0;
        let ac = triangleVertex0 - triangleVertex2;
        let cb = triangleVertex2 - triangleVertex1;
        let nor = cross(ba, ac);
        let crossBaNor = cross(ba, nor);
        let crossCbNor = cross(cb, nor);
        let crossAcNor = cross(ac, nor);
        let dot2Ba = GPUSignedDistanceFieldsDot2Vec3(ba);
        let dot2Cb = GPUSignedDistanceFieldsDot2Vec3(cb);
        let dot2Ac = GPUSignedDistanceFieldsDot2Vec3(ac);
        let dot2Nor = GPUSignedDistanceFieldsDot2Vec3(nor);
        var edges = GPUSignedDistanceFieldsGet2DEdges(triangleVertex0, triangleVertex1, triangleVertex2, triangleOffset, false);

        if (edges.gn.y < 0.0) {
            edges.gn = -edges.gn;
        }

        let nyInv = 1.0 / max(edges.gn.y, 1.0e-4);
        let dTri = -dot(edges.gn, triangleVertex0);
        let dTriProjMin = -GPUSignedDistanceFieldsOffsetByMax(dTri, edges.gn.xz, triangleOffset) * nyInv;
        let dTriProjMax = -GPUSignedDistanceFieldsOffsetByMin(dTri, edges.gn.xz, triangleOffset) * nyInv;

        var voxel : vec3<f32>;
        voxel.z = triangleAabbMin.z;
        while (voxel.z < triangleAabbMax.z) {
            voxel.x = triangleAabbMin.x;
            while (voxel.x < triangleAabbMax.x) {
                if (GPUSignedDistanceFieldsEvalEdge(voxel.xz, edges.deXZ, edges.neXZ0, edges.neXZ1, edges.neXZ2)) {
                    let y00 = -((voxel.x + 0.0) * edges.gn.x + (voxel.z + 0.0) * edges.gn.z) * nyInv;
                    let y01 = -((voxel.x + 0.0) * edges.gn.x + (voxel.z + 1.0) * edges.gn.z) * nyInv;
                    let y10 = -((voxel.x + 1.0) * edges.gn.x + (voxel.z + 0.0) * edges.gn.z) * nyInv;
                    let y11 = -((voxel.x + 1.0) * edges.gn.x + (voxel.z + 1.0) * edges.gn.z) * nyInv;
                    var minY = floor(min(y00 + dTriProjMin, min(y01 + dTriProjMin, min(y10 + dTriProjMin, y11 + dTriProjMin))));
                    minY = max(triangleAabbMin.y, minY);
                    var maxY = floor(max(y00 + dTriProjMax, max(y01 + dTriProjMax, max(y10 + dTriProjMax, y11 + dTriProjMax)))) + 1.0;
                    maxY = min(triangleAabbMax.y, maxY);

                    voxel.y = minY;
                    while (voxel.y < maxY) {
                        if (GPUSignedDistanceFieldsEvalEdge(voxel.xy, edges.deXY, edges.neXY0, edges.neXY1, edges.neXY2) &&
                            GPUSignedDistanceFieldsEvalEdge(voxel.yz, edges.deYZ, edges.neYZ0, edges.neYZ1, edges.neYZ2)) {
                            let p = voxel + vec3<f32>(0.5, 0.5, 0.5);
                            let pa = p - a;
                            let pb = p - b;
                            let pc = p - c;
                            let inside = sign(dot(crossBaNor, pa)) + sign(dot(crossCbNor, pb)) + sign(dot(crossAcNor, pc));
                            if (inside >= 2.0) {
                                let dist = dot(nor, pa) * dot(nor, pa) / max(dot2Nor, 1.0e-20) * 0.25 * 0.25;
                                ScratchBrickInterlockedMin(cascadeIndex, brickId, vec3<i32>(voxel), clamp(dist, 0.0, 1.0));
                                let samplePosition = brickMin + p * brixelSize;
                                let partialSurface = SurfaceCacheFetchTrianglePartialSurface(cascadeIndex, tri);
                                SurfaceCacheAccumulateTriangleMaterial(
                                    cascadeIndex,
                                    brickId,
                                    partialSurface,
                                    tri.jobIndex,
                                    tri.triangleIndex,
                                    tri.wp0,
                                    tri.wp1,
                                    tri.wp2,
                                    samplePosition,
                                    clamp(dist, 0.0, 1.0),
                                    brixelSize,
                                    nor,
                                    clamp((p - vec3<f32>(0.5, 0.5, 0.5)) / 7.0, vec3<f32>(0.0), vec3<f32>(0.999999)));
                            }
                        }
                        voxel.y = voxel.y + 1.0;
                    }
                }
                voxel.x = voxel.x + 1.0;
            }
            voxel.z = voxel.z + 1.0;
        }
    }
}

@compute @workgroup_size(512, 1, 1)
fn CompressBricksCS(@builtin(local_invocation_id) localInvocationID: vec3<u32>, @builtin(workgroup_id) groupId: vec3<u32>) {
    let cascadeIndex = BuildCascadeIndex(groupId.y);
    if (groupId.x >= LoadBuildInfo(cascadeIndex).compressBricksDispatchX) {
        return;
    }
    let gtid = localInvocationID.x;
    let brickListIndex = groupId.x;
    let brickId = ScratchBrickCompressionList[BrickListIndex(cascadeIndex, brickListIndex)];
    let voxelId = LoadBrickVoxelId(brickId);
    let voxelIndex = GPUSignedDistanceFieldsVoxelIdIndex(voxelId);

    if (gtid == 0u) {
        atomicStore(&gsBrickAabbMin[0], 0xffffffffu);
        atomicStore(&gsBrickAabbMin[1], 0xffffffffu);
        atomicStore(&gsBrickAabbMin[2], 0xffffffffu);
        atomicStore(&gsBrickAabbMax[0], 0u);
        atomicStore(&gsBrickAabbMax[1], 0u);
        atomicStore(&gsBrickAabbMax[2], 0u);
    }
    workgroupBarrier();

    let localCoord = GPUSignedDistanceFieldsUnflatten3D(gtid, GPUSignedDistanceFields_BrickResolution);
    let value = LoadScratchBrickDistance(cascadeIndex, brickId, vec3<i32>(localCoord));
    if (value < GPUSignedDistanceFields_BrickAabbDistanceThreshold) {
        _ = atomicMin(&gsBrickAabbMin[0], localCoord.x);
        _ = atomicMin(&gsBrickAabbMin[1], localCoord.y);
        _ = atomicMin(&gsBrickAabbMin[2], localCoord.z);
        _ = atomicMax(&gsBrickAabbMax[0], localCoord.x);
        _ = atomicMax(&gsBrickAabbMax[1], localCoord.y);
        _ = atomicMax(&gsBrickAabbMax[2], localCoord.z);
    }
    workgroupBarrier();

    if (gtid == 0u) {
        if (atomicLoad(&gsBrickAabbMin[0]) == 0xffffffffu) {
            MarkBrickFree(brickId);
            CascadeBrickMap[VoxelBase(cascadeIndex) + GPUSignedDistanceFieldsWrapFlatCoords(LoadCascadeInfo(cascadeIndex), voxelIndex)] = GPUSignedDistanceFields_InvalidId;
        } else {
            ContextBrickAabb[brickId] = GPUSignedDistanceFieldsPackBrickAabb(
                vec3<u32>(atomicLoad(&gsBrickAabbMin[0]), atomicLoad(&gsBrickAabbMin[1]), atomicLoad(&gsBrickAabbMin[2])),
                vec3<u32>(atomicLoad(&gsBrickAabbMax[0]), atomicLoad(&gsBrickAabbMax[1]), atomicLoad(&gsBrickAabbMax[2])));
        }
    }

    if (atomicLoad(&gsBrickAabbMin[0]) != 0xffffffffu) {
        if (abs(value) > 0.9999) {
            return;
        }

        let atlasValue = GPUSignedDistanceFieldsGetSign(value) * sqrt(abs(value)) * 4.0 / f32(GPUSignedDistanceFields_BrickResolution - 1u);
        StoreAtlasBrixelDistance(brickId, localCoord, clamp(atlasValue, 0.0, 1.0));
    }
}
