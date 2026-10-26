#include "sparse_voxel_types.wgsl"
#include "sparse_voxel_culling_common.wgsl"

@group(0) @binding(0) var<uniform> raster : SparseVoxelRasterUniforms;
@group(0) @binding(1) var<uniform> lod : SparseVoxelLODPassUniforms;
@group(0) @binding(2) var<storage, read_write> counters : SparseVoxelFrameCounters;
@group(0) @binding(3) var<storage, read_write> visibleInstances : array<VisibleInstance>;
@group(0) @binding(4) var<storage, read_write> visibleVoxels : array<VisibleVoxel>;
@group(0) @binding(5) var<storage, read> instances : array<SparseVoxelInstance>;
@group(0) @binding(6) var<storage, read> nodes : array<SparseVoxelLODNode>;
@group(0) @binding(7) var<storage, read> centersLengths : array<SparseVoxelCenterLength>;
@group(0) @binding(8) var<storage, read_write> queueA : array<SparseVoxelLODQueueElement>;
@group(0) @binding(9) var<storage, read_write> queueB : array<SparseVoxelLODQueueElement>;
@group(0) @binding(10) var<storage, read_write> queueCounts : array<atomic<u32>>;
@group(0) @binding(11) var<storage, read_write> lodDispatchWords : array<atomic<u32>>;
@group(0) @binding(12) var<storage, read_write> rasterIndirectArgs : array<IndirectDispatchArgs>;
@group(0) @binding(13) var<storage, read> residentMetadata : array<SparseVoxelResidentNodeMetadata>;
@group(0) @binding(14) var<storage, read_write> pageRequestBits : array<atomic<u32>>;
@group(0) @binding(15) var<storage, read_write> pageRequestPriorities : array<atomic<u32>>;
@group(0) @binding(16) var<storage, read_write> pageRequestInstances : array<atomic<u32>>;
@group(0) @binding(17) var<storage, read> lodSeeds : array<SparseVoxelLODSeed>;
@group(0) @binding(18) var<storage, read> pageBounds : array<SparseVoxelPageBounds>;
@group(0) @binding(19) var hzb : texture_2d<f32>;
@group(0) @binding(20) var<storage, read_write> rejectedNodes : array<SparseVoxelLODQueueElement>;
@group(0) @binding(21) var<storage, read_write> recoveryState : SparseVoxelLODRecoveryState;
@group(0) @binding(22) var<storage, read_write> temporalVisibleVoxels : array<VisibleVoxel>;
@group(0) @binding(23) var<storage, read> preSubmittedFlags : array<u32>;

fn lodFiniteScalar(value : f32) -> bool {
    return value == value && abs(value) < 3.402823e38;
}

fn lodFinite3(value : vec3<f32>) -> bool {
    return all(value == value) && all(abs(value) < vec3<f32>(3.402823e38));
}

fn lodFinite4(value : vec4<f32>) -> bool {
    return all(value == value) && all(abs(value) < vec4<f32>(3.402823e38));
}

fn lodReverseZ() -> bool {
    return (lod.cullingFlags & SPARSE_VOXEL_CULLING_REVERSE_Z) != 0u;
}

fn lodTemporalPhase() -> bool {
    return (lod.cullingFlags & SPARSE_VOXEL_CULLING_PREVIOUS_FRAME) != 0u;
}

fn lodRecoveryPhase() -> bool {
    return (lod.cullingFlags & SPARSE_VOXEL_CULLING_RECOVERY) != 0u;
}

fn lodMatricesEqual(a : mat4x4<f32>, b : mat4x4<f32>) -> bool {
    for (var column = 0u; column < 4u; column += 1u) {
        if (any(a[column] != b[column])) {
            return false;
        }
    }
    return true;
}

fn lodConservativeHZBDepth(uvMinimum : vec2<f32>, uvMaximum : vec2<f32>, mip : u32) -> f32 {
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
    var depth = select(0.0, 1.0, lodReverseZ());
    // Mip selection below limits the guarded rectangle to at most five
    // covered texels per axis. Visit the complete footprint instead of a
    // centered approximation so using a finer mip remains conservative.
    for (var y = 0; y < 6; y += 1) {
        for (var x = 0; x < 6; x += 1) {
            let texel = minimumTexel + vec2<i32>(x, y);
            if (texel.x > maximumCoveredTexel.x || texel.y > maximumCoveredTexel.y) {
                continue;
            }
            let sampleDepth = textureLoad(hzb, texel, i32(mip)).x;
            depth = select(max(depth, sampleDepth), min(depth, sampleDepth), lodReverseZ());
        }
    }
    return depth;
}

// Fail open unless the complete projected OBB is proven behind the current
// frame HZB. Near-plane intersections, viewport escape, invalid projections,
// and undersized history all remain visible.
fn lodOcclusionVisible(
    instanceIndex : u32,
    localMinimum : vec3<f32>,
    localMaximum : vec3<f32>
) -> bool {
    if ((lod.cullingFlags & SPARSE_VOXEL_CULLING_OCCLUSION) == 0u ||
        lod.historyValid == 0u || lod.hzbMipCount == 0u ||
        lod.hzbFirstValidMip >= lod.hzbMipCount || lod.hzbWidth == 0u || lod.hzbHeight == 0u) {
        return true;
    }
    let instance = instances[instanceIndex];
    if (lodTemporalPhase() && !lodMatricesEqual(instance.modelToWorld, instance.previousModelToWorld)) {
        // Current node bounds do not correspond to the previous-frame depth
        // source after instance motion. Keep the complete moving BLAS visible.
        return true;
    }
    let modelToWorld = instance.modelToWorld;
    var worldToOcclusionClip = raster.worldToClip;
    if (lodTemporalPhase()) {
        worldToOcclusionClip = raster.previousWorldToClip;
    }
    var uvMinimum = vec2<f32>(1.0e30);
    var uvMaximum = vec2<f32>(-1.0e30);
    var objectClosest = select(1.0, 0.0, lodReverseZ());
    for (var corner = 0u; corner < 8u; corner += 1u) {
        let local = select(localMinimum, localMaximum, vec3<bool>((corner & 1u) != 0u, (corner & 2u) != 0u, (corner & 4u) != 0u));
        let world = sparseVoxelTransformPoint(modelToWorld, local);
        if (!lodTemporalPhase()) {
            let view = sparseVoxelTransformPoint(raster.worldToCamera, world);
            if (!lodFinite3(view) || view.z <= raster.cameraWorldPositionNear.w) {
                return true;
            }
        }
        let clip = worldToOcclusionClip * vec4<f32>(world, 1.0);
        if (!lodFinite4(clip) || abs(clip.w) <= 1.0e-7) {
            return true;
        }
        // Engine perspective projections use clip.w as positive view depth.
        // Projection compatibility guarantees the previous and current near
        // values match; crossing either near plane must fail open.
        if (lodTemporalPhase() && clip.w <= raster.cameraWorldPositionNear.w) {
            return true;
        }
        let ndc = clip.xyz / clip.w;
        if (!lodFinite3(ndc)) {
            return true;
        }
        let uv = vec2<f32>(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
        uvMinimum = min(uvMinimum, uv);
        uvMaximum = max(uvMaximum, uv);
        objectClosest = select(min(objectClosest, ndc.z), max(objectClosest, ndc.z), lodReverseZ());
    }
    let uvGuard = vec2<f32>(1.0 / f32(lod.hzbWidth), 1.0 / f32(lod.hzbHeight));
    uvMinimum -= uvGuard;
    uvMaximum += uvGuard;
    if (any(uvMinimum < vec2<f32>(0.0)) || any(uvMaximum > vec2<f32>(1.0))) {
        return true;
    }
    let pixelExtent = (uvMaximum - uvMinimum) * vec2<f32>(f32(lod.hzbWidth), f32(lod.hzbHeight));
    let maximumExtent = max(pixelExtent.x, pixelExtent.y);
    if (!lodFiniteScalar(maximumExtent) || maximumExtent <= 0.0) {
        return true;
    }
    // maximumExtent is measured at full resolution, so desiredMip is already
    // an absolute mip index. Adding hzbFirstValidMip here would select a level
    // one step too coarse when mip 0 is intentionally unused. Keep the guarded
    // rectangle at <=4 texels per axis and sample every covered texel.
    let desiredMip = max(
        i32(ceil(log2(max(maximumExtent, 1.0)))) - 2 + max(lod.occlusionMipBias, 0),
        0);
    let mip = min(max(lod.hzbFirstValidMip, u32(desiredMip)), lod.hzbMipCount - 1u);
    let mipDimensions = max(textureDimensions(hzb, i32(mip)), vec2<u32>(1u));
    let fullDimensions = vec2<f32>(max(textureDimensions(hzb, 0), vec2<u32>(1u)));
    // A coarse mip covers only floor(base / 2^mip) * 2^mip pixels; the strip of
    // remainder rows and columns at the far edge is never reduced into it, so
    // nothing can be said about a node reaching into it.
    if (any(uvMaximum > sparseVoxelHZBCoveredFraction(
            fullDimensions, vec2<f32>(mipDimensions), i32(mip)))) {
        return true;
    }
    let texelsPerUV = sparseVoxelHZBTexelsPerUV(fullDimensions, i32(mip));
    let minimumTexel = clamp(
        vec2<i32>(floor(uvMinimum * texelsPerUV)),
        vec2<i32>(0), vec2<i32>(mipDimensions) - vec2<i32>(1));
    let maximumTexel = clamp(
        vec2<i32>(floor(uvMaximum * texelsPerUV)),
        vec2<i32>(0), vec2<i32>(mipDimensions) - vec2<i32>(1));
    if (any(maximumTexel - minimumTexel >= vec2<i32>(6))) {
        return true;
    }
    let hzbDepth = lodConservativeHZBDepth(uvMinimum, uvMaximum, mip);
    if (!lodFiniteScalar(hzbDepth) || !lodFiniteScalar(objectClosest)) {
        return true;
    }
    return select(
        objectClosest - lod.occlusionDepthBias <= hzbDepth,
        objectClosest + lod.occlusionDepthBias >= hzbDepth,
        lodReverseZ());
}

fn projectedNodeMetrics(instanceIndex : u32, nodeIndex : u32) -> vec2<f32> {
    let centerLength = centersLengths[nodes[nodeIndex].voxelIndex];
    let radius = centerLength.length * 0.5;
    return projectedBoundsMetrics(instanceIndex, centerLength.center - vec3<f32>(radius), centerLength.center + vec3<f32>(radius));
}

fn projectedBoundsMetrics(instanceIndex : u32, localMinimum : vec3<f32>, localMaximum : vec3<f32>) -> vec2<f32> {
    let instance = instances[instanceIndex];
    let cameraLocal = sparseVoxelTransformPoint(instance.worldToModel, raster.cameraWorldPositionNear.xyz);
    let worldCenter = sparseVoxelTransformPoint(instance.modelToWorld, (localMinimum + localMaximum) * 0.5);
    let distance = length(worldCenter - raster.cameraWorldPositionNear.xyz);

    // An aggregate octree node is a solid proxy for all occupied descendants.
    // Selecting it while the camera is inside its bounds would incorrectly
    // fill sparse/empty space (for example, the hollow synthetic shell) with
    // the node's averaged material. Force refinement until the camera is no
    // longer inside the selected node or a real leaf is reached.
    if (all(cameraLocal >= localMinimum) && all(cameraLocal <= localMaximum)) {
        return vec2<f32>(65535.0, distance);
    }

    var minimum = vec2<f32>(1.0e30);
    var maximum = vec2<f32>(-1.0e30);
    var accepted = 0u;
    for (var corner = 0u; corner < 8u; corner += 1u) {
        let local = select(localMinimum, localMaximum, vec3<bool>((corner & 4u) != 0u, (corner & 2u) != 0u, (corner & 1u) != 0u));
        let world = sparseVoxelTransformPoint(instance.modelToWorld, local);
        let camera = sparseVoxelTransformPoint(raster.worldToCamera, world);
        if (camera.z >= raster.cameraWorldPositionNear.w) {
            let pixel = raster.focalPrincipal.xy * camera.xy / camera.z;
            minimum = min(minimum, pixel);
            maximum = max(maximum, pixel);
            accepted += 1u;
        }
    }
    // A near-plane crossing has an unbounded screen projection. Using only
    // the surviving corners can underestimate its size and select a coarse
    // solid proxy directly behind or around the camera.
    if (accepted != 8u) {
        return vec2<f32>(65535.0, distance);
    }
    return vec2<f32>(max(maximum.x - minimum.x, maximum.y - minimum.y), distance);
}

fn quantizedPagePriority(metrics : vec2<f32>) -> u32 {
    let projected = u32(clamp(floor(max(metrics.x, 0.0) * 16.0 + 0.5), 0.0, 524287.0));
    let distance = u32(clamp(floor(max(metrics.y, 0.0) * 16.0 + 0.5), 0.0, 4095.0));
    return 0x80000000u | (projected << 12u) | (4095u - distance);
}

fn activeSeedCount() -> u32 {
    if ((lod.cullingFlags & SPARSE_VOXEL_LOD_SEEDS_FROM_VISIBLE_INSTANCES) != 0u) {
        return min(atomicLoad(&counters.visibleInstanceCount), lod.seedCapacity);
    }
    return min(lod.seedCount, lod.seedCapacity);
}

@compute @workgroup_size(64, 1, 1)
fn initializeLOD(@builtin(local_invocation_id) localId : vec3<u32>) {
    let index = localId.x;
    let temporalPhase = lodTemporalPhase();
    let recoveryPhase = lodRecoveryPhase();
    let levelCapacity = select(lod.levelCapacity, 8u, lod.levelCapacity == 0u);
    if (index < SPARSE_VOXEL_INDIRECT_SLOT_COUNT) {
        rasterIndirectArgs[index] = IndirectDispatchArgs(0u, 0u, 0u, 0u);
    }
    for (var level = index; level < levelCapacity; level += 64u) {
        atomicStore(&queueCounts[level], 0u);
    }
    // y and z stay resident at one so traverseLOD only has to publish the x
    // count for the next level, which lets the per-level finalize dispatch go
    // away entirely.
    for (var word = index; word < levelCapacity * 4u; word += 64u) {
        let component = word & 3u;
        atomicStore(
            &lodDispatchWords[word],
            select(0u, 1u, component == 1u || component == 2u));
    }
    for (var word = index; word < lod.requestWordCount; word += 64u) {
        atomicStore(&pageRequestBits[word], 0u);
    }
    for (var page = index; page < lod.pageCount; page += 64u) {
        atomicStore(&pageRequestPriorities[page], 0u);
        atomicStore(&pageRequestInstances[page], SPARSE_VOXEL_INVALID_INDEX);
    }
    if (temporalPhase && index == 0u) {
        atomicStore(&recoveryState.rejectedNodeCount, 0u);
        atomicStore(&recoveryState.temporalVisibleVoxelCount, 0u);
        atomicStore(&recoveryState.recoverySeedCount, 0u);
        atomicStore(&recoveryState.recoveredVisibleVoxelCount, 0u);
    }
    storageBarrier();
    let externalSeeds = (lod.cullingFlags & SPARSE_VOXEL_LOD_SEEDS_FROM_VISIBLE_INSTANCES) != 0u;
    if (!recoveryPhase && !externalSeeds && lod.seedCount == 0u && index < lod.rootCount) {
        if (index < lod.nodeCapacity) {
            queueA[index] = SparseVoxelLODQueueElement(0u, lod.firstRoot + index, 0u, 0u);
        }
    }
    let seedCount = activeSeedCount();
    if (recoveryPhase) {
        let temporalCount = min(atomicLoad(&recoveryState.temporalVisibleVoxelCount), raster.visibleVoxelCapacity);
        for (var voxelIndex = index; voxelIndex < temporalCount; voxelIndex += 64u) {
            visibleVoxels[voxelIndex] = temporalVisibleVoxels[voxelIndex];
        }
        let rejectedCount = min(atomicLoad(&recoveryState.rejectedNodeCount), lod.rejectionCapacity);
        for (var rejectedIndex = index; rejectedIndex < rejectedCount; rejectedIndex += 64u) {
            let outputIndex = atomicAdd(&queueCounts[0u], 1u);
            if (outputIndex < lod.nodeCapacity) {
                queueA[outputIndex] = rejectedNodes[rejectedIndex];
            } else {
                atomicStore(&counters.overflowStage, SPARSE_VOXEL_OVERFLOW_LOD_QUEUE);
                atomicMax(&counters.requiredCount, outputIndex + 1u);
                atomicStore(&counters.capacity, lod.nodeCapacity);
            }
        }
        // The final TLAS includes both prepass submissions and instances
        // recovered against the provisional HZB. Only the latter need fresh
        // BLAS roots; prepass instances continue from their rejected nodes.
        for (var seedIndex = index; seedIndex < seedCount; seedIndex += 64u) {
            let seed = lodSeeds[seedIndex];
            if (seed.instanceIndex == SPARSE_VOXEL_INVALID_INDEX ||
                preSubmittedFlags[seed.instanceIndex] != 0u) {
                continue;
            }
            for (var root = 0u; root < seed.rootCount; root += 1u) {
                let outputIndex = atomicAdd(&queueCounts[0u], 1u);
                if (outputIndex < lod.nodeCapacity) {
                    queueA[outputIndex] = SparseVoxelLODQueueElement(seed.instanceIndex, seed.firstRoot + root, 0u, 0u);
                } else {
                    atomicStore(&counters.overflowStage, SPARSE_VOXEL_OVERFLOW_LOD_QUEUE);
                    atomicMax(&counters.requiredCount, outputIndex + 1u);
                    atomicStore(&counters.capacity, lod.nodeCapacity);
                }
            }
        }
        atomicStore(&counters.visibleVoxelCount, temporalCount);
    } else {
        for (var seedIndex = index; seedIndex < seedCount; seedIndex += 64u) {
            let seed = lodSeeds[seedIndex];
            if (seed.instanceIndex == SPARSE_VOXEL_INVALID_INDEX || seed.firstQueueEntry >= lod.nodeCapacity) {
                continue;
            }
            let acceptedRoots = min(seed.rootCount, lod.nodeCapacity - seed.firstQueueEntry);
            if (acceptedRoots != seed.rootCount) {
                atomicStore(&counters.overflowStage, SPARSE_VOXEL_OVERFLOW_LOD_QUEUE);
                atomicMax(&counters.requiredCount, seed.firstQueueEntry + seed.rootCount);
                atomicStore(&counters.capacity, lod.nodeCapacity);
            }
            for (var root = 0u; root < acceptedRoots; root += 1u) {
                queueA[seed.firstQueueEntry + root] = SparseVoxelLODQueueElement(seed.instanceIndex, seed.firstRoot + root, 0u, 0u);
            }
            atomicMax(&queueCounts[0u], seed.firstQueueEntry + acceptedRoots);
        }
    }
    storageBarrier();
    if (index == 0u) {
        if (!recoveryPhase && !externalSeeds && lod.seedCount == 0u) {
            atomicStore(&queueCounts[0u], lod.rootCount);
            if (lod.rootCount > lod.nodeCapacity) {
                atomicStore(&counters.overflowStage, SPARSE_VOXEL_OVERFLOW_LOD_QUEUE);
                atomicMax(&counters.requiredCount, lod.rootCount);
                atomicStore(&counters.capacity, lod.nodeCapacity);
            }
        }
        let rootQueueCount = atomicLoad(&queueCounts[0u]);
        if (recoveryPhase) {
            atomicStore(&recoveryState.recoverySeedCount, rootQueueCount);
        }
        atomicStore(&lodDispatchWords[0u], (rootQueueCount + 255u) / 256u);
        atomicStore(&lodDispatchWords[1u], 1u);
        atomicStore(&lodDispatchWords[2u], 1u);
        if (!recoveryPhase) {
            atomicStore(&counters.visibleVoxelCount, 0u);
        }
        if ((lod.cullingFlags & SPARSE_VOXEL_LOD_SEEDS_FROM_VISIBLE_INSTANCES) == 0u) {
            atomicStore(&counters.visibleInstanceCount, 0u);
        }
    }
}

@compute @workgroup_size(256, 1, 1)
fn traverseLOD(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let frontierIndex = globalId.x;
    let frontierCount = atomicLoad(&queueCounts[lod.depth]);
    if (frontierIndex >= frontierCount) {
        return;
    }
    if (frontierIndex >= lod.nodeCapacity) {
        return;
    }
    var element = queueA[frontierIndex];
    if ((lod.depth & 1u) != 0u) {
        element = queueB[frontierIndex];
    }
    let nodeIndex = element.nodeIndex;
    if (nodeIndex >= lod.nodeCapacity) {
        return;
    }
    let node = nodes[nodeIndex];
    let centerLength = centersLengths[node.voxelIndex];
    let halfLength = centerLength.length * 0.5;
    let localMinimum = centerLength.center - vec3<f32>(halfLength);
    let localMaximum = centerLength.center + vec3<f32>(halfLength);
    if ((lod.cullingFlags & SPARSE_VOXEL_CULLING_FRUSTUM) != 0u &&
        !sparseVoxelFrustumVisibleLocalAABB(
            localMinimum,
            localMaximum,
            instances[element.instanceIndex].modelToWorld,
            raster)) {
        return;
    }
    if (!lodOcclusionVisible(element.instanceIndex, localMinimum, localMaximum)) {
        atomicAdd(&counters.occlusionCulledVoxelCount, 1u);
        if (lodTemporalPhase()) {
            let rejectedIndex = atomicAdd(&recoveryState.rejectedNodeCount, 1u);
            if (rejectedIndex < lod.rejectionCapacity) {
                rejectedNodes[rejectedIndex] = element;
            } else {
                atomicStore(&counters.overflowStage, SPARSE_VOXEL_OVERFLOW_LOD_QUEUE);
                atomicMax(&counters.requiredCount, rejectedIndex + 1u);
                atomicStore(&counters.capacity, lod.rejectionCapacity);
            }
        }
        return;
    }
    let metrics = projectedNodeMetrics(element.instanceIndex, nodeIndex);
    let wantsChildren = raster.lodEnabled == 0u || metrics.x * 0.5 >= raster.lodPixelThreshold;
    let chooseNode = node.childCount == 0u || !wantsChildren;
    if (lod.streamingEnabled != 0u && node.childCount == 0u && wantsChildren) {
        let pageIndex = residentMetadata[nodeIndex].refinementPageIndex;
        if (pageIndex < lod.pageCount) {
            atomicOr(&pageRequestBits[pageIndex >> 5u], 1u << (pageIndex & 31u));
            let bounds = pageBounds[pageIndex];
            let pageMetrics = projectedBoundsMetrics(element.instanceIndex, bounds.localBoundsMin.xyz, bounds.localBoundsMax.xyz);
            atomicMax(&pageRequestPriorities[pageIndex], quantizedPagePriority(pageMetrics));
            atomicMin(&pageRequestInstances[pageIndex], element.instanceIndex);
        }
    }
    if (chooseNode) {
        let outputIndex = atomicAdd(&counters.visibleVoxelCount, 1u);
        if (outputIndex < raster.visibleVoxelCapacity) {
            visibleVoxels[outputIndex] = VisibleVoxel(element.instanceIndex, node.voxelIndex, 0u, 0u);
        } else {
            atomicStore(&counters.overflowStage, SPARSE_VOXEL_OVERFLOW_VISIBLE_VOXELS);
            atomicMax(&counters.requiredCount, outputIndex + 1u);
            atomicStore(&counters.capacity, raster.visibleVoxelCapacity);
        }
        return;
    }
    for (var child = 0u; child < node.childCount; child += 1u) {
        let outputIndex = atomicAdd(&queueCounts[lod.depth + 1u], 1u);
        // Publish the next level's indirect x count here instead of in a
        // separate one-thread dispatch. Slot allocations are disjoint and
        // contiguous, so max(outputIndex) + 1 is the final count, and ceil is
        // monotonic - making this exactly the value finalizeLODLevel stored,
        // for one less dispatch and one less barrier per level.
        atomicMax(
            &lodDispatchWords[(lod.depth + 1u) * 4u],
            (outputIndex + 256u) / 256u);
        if (outputIndex < lod.nodeCapacity) {
            if ((lod.depth & 1u) == 0u) {
                queueB[outputIndex] = SparseVoxelLODQueueElement(element.instanceIndex, node.firstChild + child, 0u, 0u);
            } else {
                queueA[outputIndex] = SparseVoxelLODQueueElement(element.instanceIndex, node.firstChild + child, 0u, 0u);
            }
        } else {
            atomicStore(&counters.overflowStage, SPARSE_VOXEL_OVERFLOW_LOD_QUEUE);
            atomicMax(&counters.requiredCount, outputIndex + 1u);
            atomicStore(&counters.capacity, lod.nodeCapacity);
        }
    }
}

@compute @workgroup_size(1, 1, 1)
fn finalizeLODLevel() {
    let nextDepth = lod.depth + 1u;
    let count = atomicLoad(&queueCounts[nextDepth]);
    atomicStore(&lodDispatchWords[nextDepth * 4u], (count + 255u) / 256u);
    atomicStore(&lodDispatchWords[nextDepth * 4u + 1u], 1u);
    atomicStore(&lodDispatchWords[nextDepth * 4u + 2u], 1u);
}

@compute @workgroup_size(1, 1, 1)
fn finalizeLODSelection() {
    let count = min(atomicLoad(&counters.visibleVoxelCount), raster.visibleVoxelCapacity);
    let overflowStage = atomicLoad(&counters.overflowStage);
    if (overflowStage != SPARSE_VOXEL_OVERFLOW_NONE &&
        overflowStage != SPARSE_VOXEL_OVERFLOW_VISIBLE_VOXELS) {
        atomicStore(&counters.visibleVoxelCount, 0u);
        rasterIndirectArgs[0u] = IndirectDispatchArgs(0u, 0u, 0u, 0u);
        return;
    }
    // A selected-node overflow still leaves a valid prefix in visibleVoxels.
    // Publish that prefix instead of passing the larger atomic count to
    // preprocessVisibleVoxels, which would otherwise reject the entire frame.
    atomicStore(&counters.visibleVoxelCount, count);
    let seedCount = activeSeedCount();
    if ((lod.cullingFlags & SPARSE_VOXEL_LOD_SEEDS_FROM_VISIBLE_INSTANCES) == 0u && lod.seedCount == 0u) {
        visibleInstances[0u] = VisibleInstance(0u, 0u, count, 0u);
    } else {
        for (var seedIndex = 0u; seedIndex < seedCount; seedIndex += 1u) {
            // A single seed preserves the bootstrap range contract exactly. With
            // multiple seeds the selected voxels are append-compacted globally,
            // so per-instance ranges remain empty until a later grouping pass is
            // introduced; consumers use VisibleVoxel.instanceIndex instead.
            let selectedCount = select(0u, count, seedCount == 1u);
            visibleInstances[seedIndex] = VisibleInstance(lodSeeds[seedIndex].instanceIndex, 0u, selectedCount, 0u);
        }
    }
    let selectedInstanceCount = select(max(1u, min(lod.seedCount, lod.seedCapacity)), seedCount,
        (lod.cullingFlags & SPARSE_VOXEL_LOD_SEEDS_FROM_VISIBLE_INSTANCES) != 0u);
    atomicStore(&counters.visibleInstanceCount, select(0u, selectedInstanceCount, count != 0u));
    rasterIndirectArgs[0u] = IndirectDispatchArgs((count + 255u) / 256u, 1u, 1u, 0u);
    if (lodTemporalPhase()) {
        atomicStore(&recoveryState.temporalVisibleVoxelCount, count);
    }
    if (lodRecoveryPhase()) {
        let temporalCount = min(atomicLoad(&recoveryState.temporalVisibleVoxelCount), count);
        atomicStore(&recoveryState.recoveredVisibleVoxelCount, count - temporalCount);
    }
}

@compute @workgroup_size(256, 1, 1)
fn preserveTemporalSelection(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let index = globalId.x;
    let count = min(atomicLoad(&recoveryState.temporalVisibleVoxelCount), raster.visibleVoxelCapacity);
    if (index < count) {
        temporalVisibleVoxels[index] = visibleVoxels[index];
    }
}
