#include "sparse_voxel_types.wgsl"
#include "sparse_voxel_culling_common.wgsl"
#include "sparse_voxel_occupancy_common.wgsl"

struct SparseVoxelOccupancySource {
    sourceCapacity : u32,
    instanceCapacity : u32,
    centerCapacity : u32,
    reserved : u32,
}

@group(0) @binding(0) var<uniform> sourceState : SparseVoxelOccupancySource;
@group(0) @binding(1) var<storage, read_write> counters : SparseVoxelFrameCounters;
@group(0) @binding(2) var<storage, read> selectedLeaves : array<VisibleVoxel>;
@group(0) @binding(3) var<storage, read> instances : array<SparseVoxelInstance>;
@group(0) @binding(4) var<storage, read> centersLengths : array<SparseVoxelCenterLength>;
@group(0) @binding(5) var<storage, read_write> sourceAABBs : array<SparseVoxelOccupancyAABB>;
@group(0) @binding(6) var<uniform> occupancyState : SparseVoxelOccupancyVolume;
@group(0) @binding(7) var<storage, read> dirtyQueryBoxes : array<SparseVoxelOccupancyAABB>;
@group(0) @binding(9) var<storage, read_write> occupancyAtomicWords : array<atomic<u32>>;
@group(0) @binding(10) var<storage, read_write> denseDirtyMasks : array<atomic<u32>>;

@compute @workgroup_size(256, 1, 1)
fn transformOccupancySourceAABBs(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let sourceIndex = globalId.x;
    let sourceCount = min(
        select(
            atomicLoad(&counters.visibleVoxelCount),
            sourceState.reserved - 1u,
            sourceState.reserved != 0u),
        sourceState.sourceCapacity);
    if (sourceIndex >= sourceCount) {
        return;
    }
    let selected = selectedLeaves[sourceIndex];
    if (selected.instanceIndex >= sourceState.instanceCapacity ||
        selected.voxelIndex >= sourceState.centerCapacity) {
        return;
    }
    let instance = instances[selected.instanceIndex];
    let centerLength = centersLengths[selected.voxelIndex];
    let halfLength = centerLength.length * 0.5;
    let localMinimum = centerLength.center - vec3<f32>(halfLength);
    let localMaximum = centerLength.center + vec3<f32>(halfLength);
    var worldMinimum = vec3<f32>(3.402823e38);
    var worldMaximum = vec3<f32>(-3.402823e38);
    for (var corner = 0u; corner < 8u; corner += 1u) {
        let local = select(
            localMinimum,
            localMaximum,
            vec3<bool>(
                (corner & 1u) != 0u,
                (corner & 2u) != 0u,
                (corner & 4u) != 0u));
        let world = sparseVoxelTransformPoint(instance.modelToWorld, local);
        worldMinimum = min(worldMinimum, world);
        worldMaximum = max(worldMaximum, world);
    }
    sourceAABBs[sourceIndex] = SparseVoxelOccupancyAABB(
        vec4<f32>(worldMinimum, 0.0),
        vec4<f32>(worldMaximum, 0.0));
}

fn sourceIntersectsDirtyQuery(
    minimum : vec3<f32>,
    maximum : vec3<f32>) -> bool {
    for (var boxIndex = 0u; boxIndex < 3u; boxIndex += 1u) {
        let box = dirtyQueryBoxes[boxIndex];
        // Source splats bias both AABB endpoints outwards, so a source that
        // only touches a dirty cell boundary must survive compaction too.
        if (all(maximum >= box.minimum.xyz) &&
            all(minimum <= box.maximum.xyz)) {
            return true;
        }
    }
    return false;
}

fn occupancySourceFirstCell(
    value : f32, cellSize : f32) -> f32 {
    let scaled = value / cellSize;
    let base = floor(scaled);
    return select(base, base - 1.0, scaled == base);
}

fn occupancySourceLastCell(
    value : f32, cellSize : f32) -> f32 {
    return floor(value / cellSize);
}

fn splatFilteredOccupancyAABB(
    worldMinimum : vec3<f32>,
    worldMaximum : vec3<f32>) {
    let firstCellFloat = vec3<f32>(
        occupancySourceFirstCell(
            worldMinimum.x, occupancyState.cellSize.x),
        occupancySourceFirstCell(
            worldMinimum.y, occupancyState.cellSize.y),
        occupancySourceFirstCell(
            worldMinimum.z, occupancyState.cellSize.z));
    let lastCellFloat = vec3<f32>(
        occupancySourceLastCell(
            worldMaximum.x, occupancyState.cellSize.x),
        occupancySourceLastCell(
            worldMaximum.y, occupancyState.cellSize.y),
        occupancySourceLastCell(
            worldMaximum.z, occupancyState.cellSize.z));
    let volumeFirstFloat =
        vec3<f32>(occupancyState.originCell);
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

// The production path fuses transform, dirty-query rejection and bit splat.
// No CPU AABBs, atomic compaction, intermediate source buffer, or second
// source dispatch is required.
@compute @workgroup_size(256, 1, 1)
fn splatFilteredOccupancySources(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let sourceIndex = globalId.x;
    let sourceCount = min(
        sourceState.reserved - 1u,
        sourceState.sourceCapacity);
    if (sourceIndex >= sourceCount) {
        return;
    }
    let selected = selectedLeaves[sourceIndex];
    if (selected.instanceIndex >= sourceState.instanceCapacity ||
        selected.voxelIndex >= sourceState.centerCapacity) {
        return;
    }
    let instance = instances[selected.instanceIndex];
    let centerLength = centersLengths[selected.voxelIndex];
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
    let worldMinimum = worldCenter - worldExtent;
    let worldMaximum = worldCenter + worldExtent;
    if (!sourceIntersectsDirtyQuery(
            worldMinimum, worldMaximum)) {
        return;
    }
    splatFilteredOccupancyAABB(worldMinimum, worldMaximum);
}
