#include "sparse_voxel_types.wgsl"
#include "sparse_voxel_culling_common.wgsl"

const TLAS_WORKGROUP_SIZE : u32 = 256u;

@group(0) @binding(0) var<uniform> raster : SparseVoxelRasterUniforms;
@group(0) @binding(1) var<uniform> culling : SparseVoxelTLASCullingUniforms;
@group(0) @binding(2) var<storage, read_write> counters : SparseVoxelFrameCounters;
@group(0) @binding(3) var<storage, read> instances : array<SparseVoxelInstance>;
@group(0) @binding(4) var<storage, read> nodes : array<SparseVoxelTLASNode>;
@group(0) @binding(5) var<storage, read> tlasInstanceIndices : array<u32>;
@group(0) @binding(6) var<storage, read> blasRoots : array<SparseVoxelBLASRootRange>;
@group(0) @binding(7) var<storage, read_write> queueA : array<TLASQueueElement>;
@group(0) @binding(8) var<storage, read_write> queueB : array<TLASQueueElement>;
@group(0) @binding(9) var<storage, read_write> queueCounts : array<atomic<u32>>;
@group(0) @binding(10) var<storage, read_write> dispatchArgs : array<IndirectDispatchArgs>;
@group(0) @binding(11) var<storage, read_write> visibilityFlags : array<u32>;
@group(0) @binding(12) var<storage, read_write> visibilityOffsets : array<vec2<u32>>;
@group(0) @binding(13) var<storage, read_write> scanBlocks : array<SparseVoxelTLASScanBlock>;
@group(0) @binding(14) var<storage, read_write> visibleInstances : array<VisibleInstance>;
@group(0) @binding(15) var<storage, read_write> lodSeeds : array<SparseVoxelLODSeed>;
@group(0) @binding(16) var hzb : texture_2d<f32>;
@group(0) @binding(17) var<storage, read> preSubmittedFlags : array<u32>;
@group(0) @binding(18) var<storage, read_write> rejectionReasons : array<u32>;

var<workgroup> scanVisible : array<u32, 256>;
var<workgroup> scanRoots : array<u32, 256>;

fn recordOverflow(stage : u32, required : u32, capacity : u32) {
    atomicStore(&counters.overflowStage, stage);
    atomicMax(&counters.requiredCount, required);
    atomicStore(&counters.capacity, capacity);
}

fn frustumEnabled() -> bool {
    return (culling.cullingFlags & SPARSE_VOXEL_CULLING_FRUSTUM) != 0u;
}

fn occlusionEnabled() -> bool {
    return (culling.cullingFlags & SPARSE_VOXEL_CULLING_OCCLUSION) != 0u;
}

fn occupancyQueryEnabled() -> bool {
    return (culling.cullingFlags &
        SPARSE_VOXEL_CULLING_OCCUPANCY_QUERY) != 0u;
}

fn occupancyQueryVisibleWorldAABB(
    worldMinimum : vec3<f32>,
    worldMaximum : vec3<f32>
) -> bool {
    let count = min(culling.occupancyQueryBoxCount, 3u);
    for (var queryIndex = 0u; queryIndex < count; queryIndex += 1u) {
        let queryMinimum = culling.occupancyQueryMinimum[queryIndex].xyz;
        let queryMaximum = culling.occupancyQueryMaximum[queryIndex].xyz;
        if (all(worldMaximum >= queryMinimum) &&
            all(queryMaximum >= worldMinimum)) {
            return true;
        }
    }
    return false;
}

fn reverseZEnabled() -> bool {
    return (culling.cullingFlags & SPARSE_VOXEL_CULLING_REVERSE_Z) != 0u;
}

struct OcclusionResult {
    visible : bool,
    reason : u32,
}

fn finiteScalar(value : f32) -> bool {
    return value == value && abs(value) < 3.402823e38;
}

fn finite3(value : vec3<f32>) -> bool {
    return all(value == value) && all(abs(value) < vec3<f32>(3.402823e38));
}

fn finite4(value : vec4<f32>) -> bool {
    return all(value == value) && all(abs(value) < vec4<f32>(3.402823e38));
}

fn matricesEqual(a : mat4x4<f32>, b : mat4x4<f32>) -> bool {
    for (var column = 0u; column < 4u; column += 1u) {
        if (any(a[column] != b[column])) { return false; }
    }
    return true;
}

fn conservativeHZBRect(uvMinimum : vec2<f32>, uvMaximum : vec2<f32>, mip : u32) -> f32 {
    let dimensions = max(textureDimensions(hzb, i32(mip)), vec2<u32>(1u));
    // Texels per unit UV comes from the base dimensions, not the stored mip
    // size: the stored size rounds down and scaling UV by it stretches the mip
    // across the whole screen. See `sparseVoxelHZBTexelsPerUV`.
    let texelsPerUV = sparseVoxelHZBTexelsPerUV(
        vec2<f32>(max(textureDimensions(hzb, 0), vec2<u32>(1u))), i32(mip));
    let maximumTexel = vec2<i32>(dimensions) - vec2<i32>(1);
    let minimumTexel = clamp(
        vec2<i32>(floor(uvMinimum * texelsPerUV)),
        vec2<i32>(0), maximumTexel);
    let maximumCoveredTexel = clamp(
        vec2<i32>(floor(uvMaximum * texelsPerUV)),
        vec2<i32>(0), maximumTexel);
    var depth = select(0.0, 1.0, reverseZEnabled());
    for (var y = 0; y < 6; y += 1) {
        for (var x = 0; x < 6; x += 1) {
            let texel = minimumTexel + vec2<i32>(x, y);
            if (texel.x > maximumCoveredTexel.x || texel.y > maximumCoveredTexel.y) {
                continue;
            }
            let sampleDepth = textureLoad(hzb, texel, i32(mip)).x;
            depth = select(max(depth, sampleDepth), min(depth, sampleDepth), reverseZEnabled());
        }
    }
    return depth;
}

// A result of uncertain is always visible. Occlusion is accepted only when
// every AABB corner projects finitely inside the viewport and the conservative
// HZB depth proves the complete bound is behind existing depth.
fn occlusionVisibleWorldAABB(worldMinimum : vec3<f32>, worldMaximum : vec3<f32>) -> OcclusionResult {
    if (!occlusionEnabled()) {
        return OcclusionResult(true, SPARSE_VOXEL_OCCLUSION_VISIBLE);
    }
    if (culling.historyValid == 0u || culling.hzbMipCount == 0u ||
        culling.hzbFirstValidMip >= culling.hzbMipCount || culling.hzbWidth == 0u || culling.hzbHeight == 0u) {
        return OcclusionResult(true, SPARSE_VOXEL_OCCLUSION_HISTORY_INVALID);
    }

    var uvMinimum = vec2<f32>(1.0e30);
    var uvMaximum = vec2<f32>(-1.0e30);
    var objectClosest = select(1.0, 0.0, reverseZEnabled());
    for (var corner = 0u; corner < 8u; corner += 1u) {
        let world = select(worldMinimum, worldMaximum, vec3<bool>((corner & 1u) != 0u, (corner & 2u) != 0u, (corner & 4u) != 0u));
        let view = sparseVoxelTransformPoint(raster.worldToCamera, world);
        if (!finite3(view)) {
            return OcclusionResult(true, SPARSE_VOXEL_OCCLUSION_INVALID_PROJECTION);
        }
        if (view.z <= raster.cameraWorldPositionNear.w) {
            return OcclusionResult(true, SPARSE_VOXEL_OCCLUSION_NEAR_PLANE);
        }
        let clip = raster.worldToClip * vec4<f32>(world, 1.0);
        if (!finite4(clip) || abs(clip.w) <= 1.0e-7) {
            return OcclusionResult(true, SPARSE_VOXEL_OCCLUSION_INVALID_PROJECTION);
        }
        let ndc = clip.xyz / clip.w;
        if (!finite3(ndc)) {
            return OcclusionResult(true, SPARSE_VOXEL_OCCLUSION_INVALID_PROJECTION);
        }
        let uv = vec2<f32>(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
        uvMinimum = min(uvMinimum, uv);
        uvMaximum = max(uvMaximum, uv);
        objectClosest = select(min(objectClosest, ndc.z), max(objectClosest, ndc.z), reverseZEnabled());
    }

    if (any(uvMinimum < vec2<f32>(0.0)) || any(uvMaximum > vec2<f32>(1.0))) {
        return OcclusionResult(true, SPARSE_VOXEL_OCCLUSION_VIEWPORT_ESCAPE);
    }
    // Include a one-pixel guard band for rasterization/projection rounding. If
    // that guard leaves the viewport the result is uncertain and stays visible.
    let uvGuard = vec2<f32>(1.0 / f32(culling.hzbWidth), 1.0 / f32(culling.hzbHeight));
    uvMinimum -= uvGuard;
    uvMaximum += uvGuard;
    if (any(uvMinimum < vec2<f32>(0.0)) || any(uvMaximum > vec2<f32>(1.0))) {
        return OcclusionResult(true, SPARSE_VOXEL_OCCLUSION_VIEWPORT_ESCAPE);
    }
    let pixelExtent = (uvMaximum - uvMinimum) * vec2<f32>(f32(culling.hzbWidth), f32(culling.hzbHeight));
    let maximumExtent = max(pixelExtent.x, pixelExtent.y);
    if (!finiteScalar(maximumExtent) || maximumExtent <= 0.0) {
        return OcclusionResult(true, SPARSE_VOXEL_OCCLUSION_DEGENERATE_BOUNDS);
    }

    let desiredMip = max(
        i32(ceil(log2(max(maximumExtent, 1.0)))) - 2 + max(culling.occlusionMipBias, 0),
        0);
    let mip = min(max(culling.hzbFirstValidMip, u32(desiredMip)), culling.hzbMipCount - 1u);
    let mipDimensions = max(textureDimensions(hzb, i32(mip)), vec2<u32>(1u));
    let fullDimensions = vec2<f32>(max(textureDimensions(hzb, 0), vec2<u32>(1u)));
    // A coarse mip covers only floor(base / 2^mip) * 2^mip pixels; the strip of
    // remainder rows and columns at the far edge is never reduced into it, so
    // nothing can be said about an instance reaching into it.
    if (any(uvMaximum > sparseVoxelHZBCoveredFraction(
            fullDimensions, vec2<f32>(mipDimensions), i32(mip)))) {
        return OcclusionResult(true, SPARSE_VOXEL_OCCLUSION_DEGENERATE_BOUNDS);
    }
    let texelsPerUV = sparseVoxelHZBTexelsPerUV(fullDimensions, i32(mip));
    let minimumTexel = clamp(
        vec2<i32>(floor(uvMinimum * texelsPerUV)),
        vec2<i32>(0), vec2<i32>(mipDimensions) - vec2<i32>(1));
    let maximumTexel = clamp(
        vec2<i32>(floor(uvMaximum * texelsPerUV)),
        vec2<i32>(0), vec2<i32>(mipDimensions) - vec2<i32>(1));
    if (any(maximumTexel - minimumTexel >= vec2<i32>(6))) {
        return OcclusionResult(true, SPARSE_VOXEL_OCCLUSION_DEGENERATE_BOUNDS);
    }
    let hzbDepth = conservativeHZBRect(uvMinimum, uvMaximum, mip);
    if (!finiteScalar(hzbDepth) || !finiteScalar(objectClosest)) {
        return OcclusionResult(true, SPARSE_VOXEL_OCCLUSION_INVALID_PROJECTION);
    }
    let visible = select(
        objectClosest - culling.occlusionDepthBias <= hzbDepth,
        objectClosest + culling.occlusionDepthBias >= hzbDepth,
        reverseZEnabled());
    return OcclusionResult(visible, select(SPARSE_VOXEL_OCCLUSION_CULLED, SPARSE_VOXEL_OCCLUSION_VISIBLE, visible));
}

@compute @workgroup_size(256, 1, 1)
fn initializeTLASCulling(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let index = globalId.x;
    if (index < culling.instanceCount) {
        visibilityFlags[index] = 0u;
        visibilityOffsets[index] = vec2<u32>(0u);
        rejectionReasons[index] = select(SPARSE_VOXEL_OCCLUSION_VISIBLE, SPARSE_VOXEL_OCCLUSION_HISTORY_INVALID, occlusionEnabled() && culling.historyValid == 0u);
    }
    if (index < culling.scanBlockCount) {
        scanBlocks[index] = SparseVoxelTLASScanBlock(0u, 0u, 0u, 0u);
    }
    if (index < culling.maxDepth) {
        atomicStore(&queueCounts[index], 0u);
        dispatchArgs[index] = IndirectDispatchArgs(0u, 1u, 1u, 0u);
    }
    if (index == 0u) {
        atomicStore(&counters.visibleInstanceCount, 0u);
        atomicStore(&counters.visibleVoxelCount, 0u);
        atomicStore(&counters.candidateCount, 0u);
        atomicStore(&counters.duplicateCount, 0u);
        atomicStore(&counters.overflowStage, SPARSE_VOXEL_OVERFLOW_NONE);
        atomicStore(&counters.requiredCount, 0u);
        atomicStore(&counters.capacity, 0u);
        atomicStore(&counters.occlusionCulledVoxelCount, 0u);
        if (culling.rootNode < culling.nodeCount && culling.queueCapacity != 0u && culling.maxDepth != 0u) {
            queueA[0u] = TLASQueueElement(culling.rootNode, 0u, 0u, 0u);
            atomicStore(&queueCounts[0u], 1u);
            dispatchArgs[0u] = IndirectDispatchArgs(1u, 1u, 1u, 0u);
        }
    }
}

@compute @workgroup_size(256, 1, 1)
fn traverseTLAS(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let frontierIndex = globalId.x;
    let frontierCount = atomicLoad(&queueCounts[culling.depth]);
    if (frontierIndex >= frontierCount || frontierIndex >= culling.queueCapacity) { return; }
    var element = queueA[frontierIndex];
    if ((culling.depth & 1u) != 0u) { element = queueB[frontierIndex]; }
    if (element.nodeIndex >= culling.nodeCount) { return; }
    let node = nodes[element.nodeIndex];
    if (occupancyQueryEnabled()) {
        if (!occupancyQueryVisibleWorldAABB(
                node.worldBoundsMin.xyz, node.worldBoundsMax.xyz)) {
            return;
        }
    } else if (frustumEnabled() && !sparseVoxelFrustumVisibleWorldAABB(node.worldBoundsMin.xyz, node.worldBoundsMax.xyz, raster)) {
        return;
    }
    if (node.childCount != 0u) {
        // Previous-frame TLAS node bounds are not retained. Testing current
        // internal bounds against previous history could reject motion, so the
        // pre-cull phase deliberately performs leaf-only occlusion.
        if (!occupancyQueryEnabled() &&
            (culling.cullingFlags & SPARSE_VOXEL_CULLING_PREVIOUS_FRAME) == 0u) {
            let nodeOcclusion = occlusionVisibleWorldAABB(node.worldBoundsMin.xyz, node.worldBoundsMax.xyz);
            if (!nodeOcclusion.visible) { return; }
        }
        if (culling.depth + 1u >= culling.maxDepth) {
            recordOverflow(SPARSE_VOXEL_OVERFLOW_TLAS_QUEUE, culling.depth + 2u, culling.maxDepth);
            return;
        }
        for (var child = 0u; child < node.childCount; child += 1u) {
            let outputIndex = atomicAdd(&queueCounts[culling.depth + 1u], 1u);
            if (outputIndex < culling.queueCapacity) {
                let queued = TLASQueueElement(node.firstChild + child, culling.depth + 1u, 0u, 0u);
                if ((culling.depth & 1u) == 0u) { queueB[outputIndex] = queued; }
                else { queueA[outputIndex] = queued; }
            } else {
                recordOverflow(SPARSE_VOXEL_OVERFLOW_TLAS_QUEUE, outputIndex + 1u, culling.queueCapacity);
            }
        }
        return;
    }
    for (var offset = 0u; offset < node.instanceCount; offset += 1u) {
        let indirectIndex = node.firstInstance + offset;
        if (indirectIndex >= culling.instanceCount) { continue; }
        let instanceIndex = tlasInstanceIndices[indirectIndex];
        if (instanceIndex >= culling.instanceCount) { continue; }
        let instance = instances[instanceIndex];
        if (occupancyQueryEnabled() &&
            !occupancyQueryVisibleWorldAABB(
                instance.worldBoundsMin.xyz, instance.worldBoundsMax.xyz)) {
            continue;
        }
        if (!occupancyQueryEnabled() && frustumEnabled() && !sparseVoxelFrustumVisibleWorldAABB(instance.worldBoundsMin.xyz, instance.worldBoundsMax.xyz, raster)) {
            rejectionReasons[instanceIndex] = SPARSE_VOXEL_OCCLUSION_CULLED_FRUSTUM;
            continue;
        }
        if ((culling.cullingFlags & SPARSE_VOXEL_CULLING_RECOVERY) != 0u && preSubmittedFlags[instanceIndex] != 0u) {
            continue;
        }
        // The temporal prepass depth is an occluder for recovery work, but an
        // instance must never be rejected by depth that it submitted itself.
        // Keep it in the complete final render while the HZB tests only the
        // instances that were absent from the temporal pass.
        if ((culling.cullingFlags & SPARSE_VOXEL_CULLING_PREPASS_GUARANTEE) != 0u &&
            preSubmittedFlags[instanceIndex] != 0u) {
            visibilityFlags[instanceIndex] = 1u;
            rejectionReasons[instanceIndex] = SPARSE_VOXEL_OCCLUSION_VISIBLE;
            continue;
        }
        if ((culling.cullingFlags & SPARSE_VOXEL_CULLING_PREVIOUS_FRAME) != 0u &&
            !matricesEqual(instance.modelToWorld, instance.previousModelToWorld)) {
            visibilityFlags[instanceIndex] = 1u;
            rejectionReasons[instanceIndex] = SPARSE_VOXEL_OCCLUSION_MOVING_PREVIOUS_TRANSFORM;
            continue;
        }
        let instanceOcclusion = occlusionVisibleWorldAABB(instance.worldBoundsMin.xyz, instance.worldBoundsMax.xyz);
        rejectionReasons[instanceIndex] = instanceOcclusion.reason;
        if (instanceOcclusion.visible) { visibilityFlags[instanceIndex] = 1u; }
    }
}

@compute @workgroup_size(1, 1, 1)
fn finalizeTLASLevel() {
    let nextDepth = culling.depth + 1u;
    let count = atomicLoad(&queueCounts[nextDepth]);
    dispatchArgs[nextDepth] = IndirectDispatchArgs((min(count, culling.queueCapacity) + TLAS_WORKGROUP_SIZE - 1u) / TLAS_WORKGROUP_SIZE, 1u, 1u, 0u);
}

@compute @workgroup_size(256, 1, 1)
fn scanTLASVisibility(
    @builtin(global_invocation_id) globalId : vec3<u32>,
    @builtin(local_invocation_id) localId : vec3<u32>,
    @builtin(workgroup_id) workgroupId : vec3<u32>
) {
    let instanceIndex = globalId.x;
    let lane = localId.x;
    var visible = 0u;
    var roots = 0u;
    if (instanceIndex < culling.instanceCount) {
        visible = visibilityFlags[instanceIndex];
        if (visible != 0u) {
            let blasIndex = instances[instanceIndex].blasIndex;
            if (blasIndex < culling.blasCount) { roots = blasRoots[blasIndex].rootCount; }
        }
    }
    scanVisible[lane] = visible;
    scanRoots[lane] = roots;
    workgroupBarrier();
    for (var step = 1u; step < TLAS_WORKGROUP_SIZE; step <<= 1u) {
        var addVisible = 0u;
        var addRoots = 0u;
        if (lane >= step) {
            addVisible = scanVisible[lane - step];
            addRoots = scanRoots[lane - step];
        }
        workgroupBarrier();
        scanVisible[lane] += addVisible;
        scanRoots[lane] += addRoots;
        workgroupBarrier();
    }
    if (instanceIndex < culling.instanceCount) {
        visibilityOffsets[instanceIndex] = vec2<u32>(scanVisible[lane] - visible, scanRoots[lane] - roots);
    }
    if (lane == TLAS_WORKGROUP_SIZE - 1u) {
        scanBlocks[workgroupId.x] = SparseVoxelTLASScanBlock(scanVisible[lane], scanRoots[lane], 0u, 0u);
    }
}

@compute @workgroup_size(256, 1, 1)
fn scanTLASBlockSums(@builtin(local_invocation_id) localId : vec3<u32>) {
    let lane = localId.x;
    var visible = 0u;
    var roots = 0u;
    if (lane < culling.scanBlockCount) {
        visible = scanBlocks[lane].visible;
        roots = scanBlocks[lane].roots;
    }
    scanVisible[lane] = visible;
    scanRoots[lane] = roots;
    workgroupBarrier();
    for (var step = 1u; step < TLAS_WORKGROUP_SIZE; step <<= 1u) {
        var addVisible = 0u;
        var addRoots = 0u;
        if (lane >= step) {
            addVisible = scanVisible[lane - step];
            addRoots = scanRoots[lane - step];
        }
        workgroupBarrier();
        scanVisible[lane] += addVisible;
        scanRoots[lane] += addRoots;
        workgroupBarrier();
    }
    if (lane < culling.scanBlockCount) {
        scanBlocks[lane].visible = scanVisible[lane] - visible;
        scanBlocks[lane].roots = scanRoots[lane] - roots;
    }
    if (lane == culling.scanBlockCount - 1u) {
        let totalVisible = scanVisible[lane];
        let totalRoots = scanRoots[lane];
        if (totalVisible > culling.visibleInstanceCapacity) {
            recordOverflow(SPARSE_VOXEL_OVERFLOW_VISIBLE_INSTANCES, totalVisible, culling.visibleInstanceCapacity);
        } else if (totalRoots > culling.lodQueueCapacity) {
            recordOverflow(SPARSE_VOXEL_OVERFLOW_LOD_QUEUE, totalRoots, culling.lodQueueCapacity);
        } else if (atomicLoad(&counters.overflowStage) == SPARSE_VOXEL_OVERFLOW_NONE) {
            atomicStore(&counters.visibleInstanceCount, totalVisible);
        }
    }
}

@compute @workgroup_size(256, 1, 1)
fn scatterTLASVisibility(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let instanceIndex = globalId.x;
    if (instanceIndex >= culling.instanceCount || visibilityFlags[instanceIndex] == 0u || atomicLoad(&counters.overflowStage) != SPARSE_VOXEL_OVERFLOW_NONE) { return; }
    let block = instanceIndex / TLAS_WORKGROUP_SIZE;
    let localOffset = visibilityOffsets[instanceIndex];
    let outputIndex = scanBlocks[block].visible + localOffset.x;
    let rootOffset = scanBlocks[block].roots + localOffset.y;
    let blasIndex = instances[instanceIndex].blasIndex;
    if (outputIndex >= culling.visibleInstanceCapacity || blasIndex >= culling.blasCount) { return; }
    let roots = blasRoots[blasIndex];
    visibleInstances[outputIndex] = VisibleInstance(instanceIndex, 0u, 0u, 0u);
    lodSeeds[outputIndex] = SparseVoxelLODSeed(instanceIndex, roots.firstRoot, roots.rootCount, rootOffset);
}
