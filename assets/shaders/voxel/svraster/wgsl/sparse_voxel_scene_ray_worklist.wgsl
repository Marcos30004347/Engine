#include "sparse_voxel_scene_ray_types.wgsl"

struct SparseVoxelSceneRayWorklistSettings {
    rayCapacity : u32,
    reserved0 : u32,
    reserved1 : u32,
    reserved2 : u32,
}

@group(0) @binding(0) var<uniform> settings : SparseVoxelSceneRayWorklistSettings;
@group(0) @binding(1) var<storage, read> candidates : array<SparseVoxelSceneRay>;
@group(0) @binding(2) var<storage, read_write> activeIndices : array<u32>;
// xyz are DispatchIndirectArgs; w is the exact active-lane count.
@group(0) @binding(3) var<storage, read_write> workState : array<atomic<u32>>;
@group(0) @binding(4) var<storage, read_write> finalRays : array<SparseVoxelSceneRay>;
@group(0) @binding(5) var<storage, read_write> finalReservoirs : array<SparseVoxelDIReservoir>;
// Optional second indirect record. Per-pixel ReSTIR owns both the final-ray
// list and the scheduled-receiver list; clearing them in one dispatch avoids a
// second one-thread command buffer and synchronization boundary.
@group(0) @binding(6) var<storage, read_write> secondaryWorkState : array<atomic<u32>>;

var<workgroup> groupActiveCount : atomic<u32>;
var<workgroup> groupBase : u32;

@compute @workgroup_size(1, 1, 1)
fn resetSparseVoxelSceneRayWorklist(
    @builtin(global_invocation_id) gid : vec3<u32>) {
    if (gid.x != 0u) { return; }
    atomicStore(&workState[0], 0u);
    atomicStore(&workState[1], 1u);
    atomicStore(&workState[2], 1u);
    atomicStore(&workState[3], 0u);
    if (settings.reserved0 != 0u && arrayLength(&secondaryWorkState) >= 4u) {
        atomicStore(&secondaryWorkState[0], 0u);
        atomicStore(&secondaryWorkState[1], 1u);
        atomicStore(&secondaryWorkState[2], 1u);
        atomicStore(&secondaryWorkState[3], 0u);
    }
}

@compute @workgroup_size(1, 1, 1)
fn finalizeSparseVoxelSceneRayWorklist(
    @builtin(global_invocation_id) gid : vec3<u32>) {
    if (gid.x != 0u) { return; }
    let activeCount = atomicLoad(&workState[3]);
    atomicStore(&workState[0], (activeCount + 63u) / 64u);
    atomicStore(&workState[1], 1u);
    atomicStore(&workState[2], 1u);
}

// Build one compact list for every expensive ray-domain pass. A workgroup
// performs a local atomic allocation and reserves one global range, reducing
// global counter contention from one atomic per ray to one per 64 rays.
@compute @workgroup_size(64, 1, 1)
fn buildSparseVoxelSceneRayWorklist(
    @builtin(global_invocation_id) gid : vec3<u32>,
    @builtin(local_invocation_index) localIndex : u32) {
    if (localIndex == 0u) {
        atomicStore(&groupActiveCount, 0u);
        groupBase = 0u;
    }
    workgroupBarrier();

    let canonicalIndex = gid.x;
    let inBounds =
        canonicalIndex < settings.rayCapacity &&
        canonicalIndex < arrayLength(&candidates);
    let laneIsActive = inBounds &&
        (candidates[canonicalIndex].seedFlags.y &
            SPARSE_VOXEL_SCENE_RAY_FLAG_ACTIVE) != 0u;
    var localOffset = 0u;
    if (laneIsActive) {
        localOffset = atomicAdd(&groupActiveCount, 1u);
    } else if (inBounds) {
        // These are the only authoritative fields downstream uses to decide
        // whether a canonical slot belongs to this frame. Clearing them here
        // lets every later pass consume only activeIndices without exposing
        // stale frame-local records.
        if (canonicalIndex < arrayLength(&finalRays)) {
            finalRays[canonicalIndex].seedFlags.y = 0u;
        }
        if (canonicalIndex < arrayLength(&finalReservoirs)) {
            finalReservoirs[canonicalIndex].state.z = 0u;
        }
    }
    workgroupBarrier();

    if (localIndex == 0u) {
        let count = atomicLoad(&groupActiveCount);
        if (count != 0u) {
            groupBase = atomicAdd(&workState[3], count);
            atomicMax(&workState[0], (groupBase + count + 63u) / 64u);
        }
    }
    workgroupBarrier();

    if (laneIsActive) {
        let outputIndex = groupBase + localOffset;
        if (outputIndex < arrayLength(&activeIndices)) {
            activeIndices[outputIndex] = canonicalIndex;
        }
    }
}
