#include "sparse_voxel_types.wgsl"
#include "sparse_voxel_culling_common.wgsl"
#include "sparse_voxel_occupancy_common.wgsl"

struct SparseVoxelOccupancySceneTraversal {
    instanceCount : u32,
    hierarchyNodeCount : u32,
    blasCount : u32,
    traversalLevelCount : u32,
    queueCapacity : u32,
    queryBoxCount : u32,
    depth : u32,
    reserved : u32,
}

@group(0) @binding(0) var<uniform> traversal : SparseVoxelOccupancySceneTraversal;
@group(0) @binding(1) var<uniform> occupancyState : SparseVoxelOccupancyVolume;
@group(0) @binding(2) var<storage, read> dirtyQueryBoxes : array<SparseVoxelOccupancyAABB>;
@group(0) @binding(3) var<storage, read_write> denseDirtyMasks : array<atomic<u32>>;
@group(0) @binding(4) var<storage, read_write> occupancyAtomicWords : array<atomic<u32>>;
@group(0) @binding(5) var<storage, read> instances : array<SparseVoxelInstance>;
@group(0) @binding(6) var<storage, read> hierarchyNodes : array<SparseVoxelLODNode>;
@group(0) @binding(7) var<storage, read> centersLengths : array<SparseVoxelCenterLength>;
@group(0) @binding(8) var<storage, read> blasRoots : array<SparseVoxelBLASRootRange>;
@group(0) @binding(9) var<storage, read_write> queueA : array<SparseVoxelLODQueueElement>;
@group(0) @binding(10) var<storage, read_write> queueB : array<SparseVoxelLODQueueElement>;
@group(0) @binding(11) var<storage, read_write> queueCounts : array<atomic<u32>>;
@group(0) @binding(12) var<storage, read_write> dispatchArgs : array<IndirectDispatchArgs>;

fn occupancySceneIntersectsDirtyQuery(
    minimum : vec3<f32>,
    maximum : vec3<f32>) -> bool {
    for (var boxIndex = 0u;
         boxIndex < min(traversal.queryBoxCount, 3u);
         boxIndex += 1u) {
        let box = dirtyQueryBoxes[boxIndex];
        if (all(maximum >= box.minimum.xyz) &&
            all(minimum <= box.maximum.xyz)) {
            return true;
        }
    }
    return false;
}

fn occupancySceneNodeWorldBounds(
    instance : SparseVoxelInstance,
    centerLength : SparseVoxelCenterLength) -> SparseVoxelOccupancyAABB {
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
    return SparseVoxelOccupancyAABB(
        vec4<f32>(worldCenter - worldExtent, 0.0),
        vec4<f32>(worldCenter + worldExtent, 0.0));
}

fn occupancySceneFirstCell(value : f32, cellSize : f32) -> f32 {
    let scaled = value / cellSize;
    let base = floor(scaled);
    return select(base, base - 1.0, scaled == base);
}

fn occupancySceneLastCell(value : f32, cellSize : f32) -> f32 {
    return floor(value / cellSize);
}

fn occupancySceneSplatLeaf(bounds : SparseVoxelOccupancyAABB) {
    let firstCellFloat = vec3<f32>(
        occupancySceneFirstCell(
            bounds.minimum.x, occupancyState.cellSize.x),
        occupancySceneFirstCell(
            bounds.minimum.y, occupancyState.cellSize.y),
        occupancySceneFirstCell(
            bounds.minimum.z, occupancyState.cellSize.z));
    let lastCellFloat = vec3<f32>(
        occupancySceneLastCell(
            bounds.maximum.x, occupancyState.cellSize.x),
        occupancySceneLastCell(
            bounds.maximum.y, occupancyState.cellSize.y),
        occupancySceneLastCell(
            bounds.maximum.z, occupancyState.cellSize.z));
    let volumeFirstFloat = vec3<f32>(occupancyState.originCell);
    let volumeLast = occupancyState.originCell +
        vec3<i32>(occupancyState.logicalResolution) -
        vec3<i32>(1);
    let volumeLastFloat = vec3<f32>(volumeLast);
    if (any(firstCellFloat > volumeLastFloat) ||
        any(lastCellFloat < volumeFirstFloat)) {
        return;
    }
    let firstCell =
        vec3<i32>(max(firstCellFloat, volumeFirstFloat));
    let lastCell =
        vec3<i32>(min(lastCellFloat, volumeLastFloat));

    var z = firstCell.z;
    loop {
        var y = firstCell.y;
        loop {
            var x = firstCell.x;
            loop {
                let logical = vec3<u32>(
                    vec3<i32>(x, y, z) -
                    occupancyState.originCell);
                let address = sparseVoxelOccupancyAddress(
                    logical, occupancyState);
                if ((atomicLoad(&denseDirtyMasks[address.wordIndex]) &
                     address.bitMask) != 0u) {
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
fn clearOccupancySceneTraversal(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let level = globalId.x;
    if (level >= traversal.traversalLevelCount) {
        return;
    }
    atomicStore(&queueCounts[level], 0u);
    dispatchArgs[level] = IndirectDispatchArgs(0u, 1u, 1u, 0u);
}

@compute @workgroup_size(64, 1, 1)
fn seedOccupancySceneTraversal(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let instanceIndex = globalId.x;
    if (traversal.queryBoxCount == 0u ||
        instanceIndex >= traversal.instanceCount) {
        return;
    }
    let instance = instances[instanceIndex];
    if (!occupancySceneIntersectsDirtyQuery(
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
        }
    }
}

@compute @workgroup_size(64, 1, 1)
fn traverseOccupancyScene(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let frontierIndex = globalId.x;
    if (traversal.depth >= traversal.traversalLevelCount ||
        frontierIndex >= atomicLoad(&queueCounts[traversal.depth]) ||
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
    let node = hierarchyNodes[element.nodeIndex];
    if (node.voxelIndex >= traversal.hierarchyNodeCount) {
        return;
    }
    let bounds = occupancySceneNodeWorldBounds(
        instances[element.instanceIndex],
        centersLengths[node.voxelIndex]);
    if (!occupancySceneIntersectsDirtyQuery(
            bounds.minimum.xyz, bounds.maximum.xyz)) {
        return;
    }

    if (node.childCount == 0u) {
        occupancySceneSplatLeaf(bounds);
        return;
    }
    if (traversal.depth + 1u >=
        traversal.traversalLevelCount) {
        // Valid hierarchies cannot reach this path. Fail conservatively if a
        // malformed or partially published hierarchy does: the internal node
        // contains at least one occupied descendant.
        occupancySceneSplatLeaf(bounds);
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
        }
    }
}

@compute @workgroup_size(1, 1, 1)
fn finalizeOccupancySceneTraversal() {
    if (traversal.depth >= traversal.traversalLevelCount) {
        return;
    }
    let count = min(
        atomicLoad(&queueCounts[traversal.depth]),
        traversal.queueCapacity);
    dispatchArgs[traversal.depth] = IndirectDispatchArgs(
        (count + 63u) / 64u, 1u, 1u, 0u);
}
