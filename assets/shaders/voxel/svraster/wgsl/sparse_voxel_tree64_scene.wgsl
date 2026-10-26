#include "sparse_voxel_types.wgsl"
#include "sparse_voxel_culling_common.wgsl"

struct SparseVoxelTree64BuildJob {
    worldChunk : vec3<i32>,
    nodeSlot : u32,
    contentGeneration : u32,
    flags : u32,
    reserved0 : u32,
    reserved1 : u32,
}

struct SparseVoxelTree64SceneTraversal {
    instanceCount : u32,
    hierarchyNodeCount : u32,
    blasCount : u32,
    traversalLevelCount : u32,
    queueCapacity : u32,
    chunkCount : u32,
    chunkSide : u32,
    cellCount : u32,
    cellCapacity : u32,
    depth : u32,
    cellWordCount : u32,
    cellWordCapacity : u32,
    cellSize : vec3<f32>,
    reserved2 : u32,
}

struct SparseVoxelTree64SceneCounters {
    queueOverflow : atomic<u32>,
    instancesTested : atomic<u32>,
    nodesTested : atomic<u32>,
    leavesReached : atomic<u32>,
    chunkSplats : atomic<u32>,
    cellsWritten : atomic<u32>,
    reserved0 : atomic<u32>,
    reserved1 : atomic<u32>,
}

struct SparseVoxelTree64SceneAABB {
    minimum : vec4<f32>,
    maximum : vec4<f32>,
}

@group(0) @binding(0) var<uniform> traversal : SparseVoxelTree64SceneTraversal;
@group(0) @binding(1) var<storage, read> jobs : array<SparseVoxelTree64BuildJob>;
@group(0) @binding(2) var<storage, read_write> cells : array<atomic<u32>>;
@group(0) @binding(3) var<storage, read> instances : array<SparseVoxelInstance>;
@group(0) @binding(4) var<storage, read> hierarchyNodes : array<SparseVoxelLODNode>;
@group(0) @binding(5) var<storage, read> centersLengths : array<SparseVoxelCenterLength>;
@group(0) @binding(6) var<storage, read> blasRoots : array<SparseVoxelBLASRootRange>;
@group(0) @binding(7) var<storage, read_write> queueA : array<SparseVoxelLODQueueElement>;
@group(0) @binding(8) var<storage, read_write> queueB : array<SparseVoxelLODQueueElement>;
@group(0) @binding(9) var<storage, read_write> queueCounts : array<atomic<u32>>;
@group(0) @binding(10) var<storage, read_write> dispatchArgs : array<IndirectDispatchArgs>;
@group(0) @binding(11) var<storage, read_write> counters : SparseVoxelTree64SceneCounters;

const TREE64_SCENE_JOB_VALID = 1u;

fn tree64SceneChunkMinimum(jobIndex : u32) -> vec3<f32> {
    return vec3<f32>(jobs[jobIndex].worldChunk) *
        f32(traversal.chunkSide) * traversal.cellSize;
}

fn tree64SceneIntersectsChunk(
    minimum : vec3<f32>,
    maximum : vec3<f32>,
    jobIndex : u32) -> bool {
    let chunkMinimum = tree64SceneChunkMinimum(jobIndex);
    let chunkMaximum = chunkMinimum +
        vec3<f32>(f32(traversal.chunkSide)) *
        traversal.cellSize;
    return all(maximum >= chunkMinimum) &&
        all(minimum <= chunkMaximum);
}

fn tree64SceneIntersectsAnyChunk(
    minimum : vec3<f32>,
    maximum : vec3<f32>) -> bool {
    for (var jobIndex = 0u;
         jobIndex < traversal.chunkCount;
         jobIndex += 1u) {
        if ((jobs[jobIndex].flags & TREE64_SCENE_JOB_VALID) != 0u &&
            tree64SceneIntersectsChunk(
                minimum, maximum, jobIndex)) {
            return true;
        }
    }
    return false;
}

fn tree64SceneNodeWorldBounds(
    instance : SparseVoxelInstance,
    centerLength : SparseVoxelCenterLength)
    -> SparseVoxelTree64SceneAABB {
    let worldCenter = sparseVoxelTransformPoint(
        instance.modelToWorld, centerLength.center);
    let halfLength = centerLength.length * 0.5;
    let worldExtent = halfLength * vec3<f32>(
        abs(instance.modelToWorld[0].x) +
            abs(instance.modelToWorld[1].x) +
            abs(instance.modelToWorld[2].x),
        abs(instance.modelToWorld[0].y) +
            abs(instance.modelToWorld[1].y) +
            abs(instance.modelToWorld[2].y),
        abs(instance.modelToWorld[0].z) +
            abs(instance.modelToWorld[1].z) +
            abs(instance.modelToWorld[2].z));
    return SparseVoxelTree64SceneAABB(
        vec4<f32>(worldCenter - worldExtent, 0.0),
        vec4<f32>(worldCenter + worldExtent, 0.0));
}

fn tree64SceneSplatLeaf(bounds : SparseVoxelTree64SceneAABB) {
    for (var jobIndex = 0u;
         jobIndex < traversal.chunkCount;
         jobIndex += 1u) {
        if ((jobs[jobIndex].flags & TREE64_SCENE_JOB_VALID) == 0u ||
            !tree64SceneIntersectsChunk(
                bounds.minimum.xyz,
                bounds.maximum.xyz,
                jobIndex)) {
            continue;
        }
        atomicAdd(&counters.chunkSplats, 1u);
        let chunkMinimum = tree64SceneChunkMinimum(jobIndex);
        let begin = clamp(
            vec3<i32>(floor(
                (bounds.minimum.xyz - chunkMinimum) /
                traversal.cellSize)),
            vec3<i32>(0),
            vec3<i32>(i32(traversal.chunkSide)));
        let end = clamp(
            vec3<i32>(ceil(
                (bounds.maximum.xyz - chunkMinimum) /
                traversal.cellSize)),
            vec3<i32>(0),
            vec3<i32>(i32(traversal.chunkSide)));
        if (any(begin >= end)) {
            continue;
        }
        for (var z = begin.z; z < end.z; z += 1) {
            for (var y = begin.y; y < end.y; y += 1) {
                for (var x = begin.x; x < end.x; x += 1) {
                    let local = vec3<u32>(
                        vec3<i32>(x, y, z));
                    let localCellIndex =
                        local.x + traversal.chunkSide *
                        (local.y + traversal.chunkSide * local.z);
                    let wordIndex =
                        jobIndex * traversal.cellWordCount +
                        localCellIndex / 32u;
                    if (wordIndex < traversal.cellWordCapacity) {
                        atomicOr(
                            &cells[wordIndex],
                            1u << (localCellIndex & 31u));
                        atomicAdd(&counters.cellsWritten, 1u);
                    }
                }
            }
        }
    }
}

@compute @workgroup_size(64, 1, 1)
fn clearTree64SceneTraversal(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let index = globalId.x;
    if (index < traversal.traversalLevelCount) {
        atomicStore(&queueCounts[index], 0u);
        dispatchArgs[index] =
            IndirectDispatchArgs(0u, 1u, 1u, 0u);
    } else if (index == traversal.traversalLevelCount) {
        dispatchArgs[index] =
            IndirectDispatchArgs(0u, 1u, 1u, 0u);
    }
    if (index == 0u) {
        atomicStore(&counters.queueOverflow, 0u);
        atomicStore(&counters.instancesTested, 0u);
        atomicStore(&counters.nodesTested, 0u);
        atomicStore(&counters.leavesReached, 0u);
        atomicStore(&counters.chunkSplats, 0u);
        atomicStore(&counters.cellsWritten, 0u);
        atomicStore(&counters.reserved0, 0u);
        atomicStore(&counters.reserved1, 0u);
    }
}

@compute @workgroup_size(64, 1, 1)
fn seedTree64SceneTraversal(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let instanceIndex = globalId.x;
    if (traversal.chunkCount == 0u ||
        instanceIndex >= traversal.instanceCount) {
        return;
    }
    atomicAdd(&counters.instancesTested, 1u);
    let instance = instances[instanceIndex];
    if (!tree64SceneIntersectsAnyChunk(
            instance.worldBoundsMin.xyz,
            instance.worldBoundsMax.xyz) ||
        instance.blasIndex >= traversal.blasCount) {
        return;
    }
    let roots = blasRoots[instance.blasIndex];
    for (var root = 0u; root < roots.rootCount; root += 1u) {
        let nodeIndex = roots.firstRoot + root;
        if (nodeIndex >= traversal.hierarchyNodeCount) {
            continue;
        }
        let outputIndex = atomicAdd(&queueCounts[0u], 1u);
        if (outputIndex < traversal.queueCapacity) {
            queueA[outputIndex] = SparseVoxelLODQueueElement(
                instanceIndex, nodeIndex, 0u, 0u);
        } else {
            atomicStore(&counters.queueOverflow, 1u);
        }
    }
}

@compute @workgroup_size(64, 1, 1)
fn traverseTree64Scene(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let frontierIndex = globalId.x;
    if (traversal.depth >= traversal.traversalLevelCount ||
        frontierIndex >=
            atomicLoad(&queueCounts[traversal.depth]) ||
        frontierIndex >= traversal.queueCapacity) {
        return;
    }
    var element = queueA[frontierIndex];
    if ((traversal.depth & 1u) != 0u) {
        element = queueB[frontierIndex];
    }
    if (element.instanceIndex >= traversal.instanceCount ||
        element.nodeIndex >= traversal.hierarchyNodeCount) {
        return;
    }
    atomicAdd(&counters.nodesTested, 1u);
    let node = hierarchyNodes[element.nodeIndex];
    if (node.voxelIndex >= traversal.hierarchyNodeCount) {
        return;
    }
    let bounds = tree64SceneNodeWorldBounds(
        instances[element.instanceIndex],
        centersLengths[node.voxelIndex]);
    if (!tree64SceneIntersectsAnyChunk(
            bounds.minimum.xyz, bounds.maximum.xyz)) {
        return;
    }
    if (node.childCount == 0u ||
        traversal.depth + 1u >=
            traversal.traversalLevelCount) {
        atomicAdd(&counters.leavesReached, 1u);
        tree64SceneSplatLeaf(bounds);
        return;
    }
    for (var child = 0u; child < node.childCount; child += 1u) {
        let childIndex = node.firstChild + child;
        if (childIndex >= traversal.hierarchyNodeCount) {
            continue;
        }
        let outputIndex = atomicAdd(
            &queueCounts[traversal.depth + 1u], 1u);
        if (outputIndex < traversal.queueCapacity) {
            let queued = SparseVoxelLODQueueElement(
                element.instanceIndex, childIndex, 0u, 0u);
            if ((traversal.depth & 1u) == 0u) {
                queueB[outputIndex] = queued;
            } else {
                queueA[outputIndex] = queued;
            }
        } else {
            atomicStore(&counters.queueOverflow, 1u);
        }
    }
}

@compute @workgroup_size(1, 1, 1)
fn finalizeTree64SceneTraversal() {
    if (traversal.depth >= traversal.traversalLevelCount) {
        return;
    }
    let count = min(
        atomicLoad(&queueCounts[traversal.depth]),
        traversal.queueCapacity);
    dispatchArgs[traversal.depth] = IndirectDispatchArgs(
        (count + 63u) / 64u, 1u, 1u, 0u);
}

@compute @workgroup_size(1, 1, 1)
fn prepareFailClosedTree64SceneTraversal() {
    let activeCount =
        traversal.chunkCount * traversal.cellWordCount;
    let groups = select(
        0u,
        (activeCount + 63u) / 64u,
        atomicLoad(&counters.queueOverflow) != 0u);
    dispatchArgs[traversal.traversalLevelCount] =
        IndirectDispatchArgs(groups, 1u, 1u, 0u);
}

@compute @workgroup_size(64, 1, 1)
fn failClosedTree64SceneTraversal(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    if (atomicLoad(&counters.queueOverflow) == 0u) {
        return;
    }
    let index = globalId.x;
    let activeCount =
        traversal.chunkCount * traversal.cellWordCount;
    if (index < activeCount &&
        index < traversal.cellWordCapacity) {
        atomicStore(&cells[index], 0xffffffffu);
    }
}
