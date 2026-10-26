#include "sparse_voxel_scene_ray_types.wgsl"
#include "sparse_voxel_tree64_common.wgsl"

struct SparseVoxelTree64Ray {
    // xyz: origin, w: maximum distance measured from that origin.
    originMaximumDistance : vec4<f32>,
    // xyz: normalized direction. w carries the distance this ray was advanced
    // from the original surface origin, so the merge pass can convert the
    // traversal's local hit distance back to an absolute one. The traversal
    // itself never reads this lane.
    direction : vec4<f32>,
}

struct SparseVoxelSceneRayDispatch { rayCount : u32, reserved0 : u32, reserved1 : u32, reserved2 : u32, }

@group(0) @binding(0) var<uniform> dispatch : SparseVoxelSceneRayDispatch;
@group(0) @binding(1) var<storage, read> rays : array<SparseVoxelSceneRay>;
@group(0) @binding(2) var<storage, read> screenHits : array<SparseVoxelSceneRayHit>;
@group(0) @binding(3) var<storage, read_write> treeRays : array<SparseVoxelTree64Ray>;
@group(0) @binding(4) var<storage, read> treeState : SparseVoxelTree64State;

// Reject only the Tree64 cell containing the receiver. The canonical light ray
// keeps the exact reconstructed origin; this distance is local traversal state
// and is never fed back into temporal or spatial reservoir reconnection.
fn tree64_receiver_cell_exit(
    origin : vec3<f32>, direction : vec3<f32>) -> f32 {
    var exit = 3.402823466e+38f;
    var found = false;
    for (var axis = 0u; axis < 3u; axis += 1u) {
        let cellSize = treeState.cellSize[axis];
        if (cellSize <= 0.0) { continue; }
        let step = direction[axis];
        if (abs(step) < 1.0e-12) { continue; }
        let coordinate = origin[axis] / cellSize;
        let cell = floor(coordinate);
        let boundary = select(cell, cell + 1.0, step > 0.0) * cellSize;
        exit = min(exit, (boundary - origin[axis]) / step);
        found = true;
    }
    return select(0.0, max(exit, 0.0), found);
}

@compute @workgroup_size(64, 1, 1)
fn prepareSparseVoxelSceneTree64Rays(@builtin(global_invocation_id) gid : vec3<u32>) {
    let index = gid.x;
    if (index >= dispatch.rayCount || index >= arrayLength(&rays) || index >= arrayLength(&screenHits) || index >= arrayLength(&treeRays)) { return; }
    let ray = rays[index];
    let screenHit = screenHits[index];
    let unresolved = (ray.seedFlags.y & SPARSE_VOXEL_SCENE_RAY_FLAG_ACTIVE) != 0u && screenHit.status != SPARSE_VOXEL_SCENE_RAY_STATUS_HIT;
    if (!unresolved) {
        treeRays[index] = SparseVoxelTree64Ray(vec4<f32>(0.0, 0.0, 0.0, -1.0), vec4<f32>(0.0));
        return;
    }
    let origin = ray.originMinimumDistance.xyz;
    let direction = normalize(ray.directionMaximumDistance.xyz);
    let maximumDistance = ray.directionMaximumDistance.w;

    var startDistance = tree64_receiver_cell_exit(origin, direction);

    // Resume where the screen tracer stopped. Only ESCAPED_SCENE means it
    // genuinely ran out of screen with the span behind it already tested;
    // MISS_TMAX means it marched coarsely to the end and its thickness test
    // can miss occluders, so that span is re-traced in world space.
    if (screenHit.status == SPARSE_VOXEL_SCENE_RAY_STATUS_ESCAPED_SCENE &&
        screenHit.identity.z == SPARSE_VOXEL_SCENE_RAY_BACKEND_SCREEN) {
        // Step back by one cell so nothing falls in the seam between the two
        // tracers: the screen march tests discrete points, and an occluder
        // straddling the hand-off point must be seen by one of them.
        let overlap = max(
            max(treeState.cellSize[0], treeState.cellSize[1]),
            treeState.cellSize[2]);
        startDistance = max(
            startDistance, screenHit.hitPositionDistance.w - overlap);
    }

    startDistance = clamp(startDistance, 0.0, max(maximumDistance, 0.0));
    treeRays[index] = SparseVoxelTree64Ray(
        vec4<f32>(
            origin + direction * startDistance,
            max(maximumDistance - startDistance, 0.0)),
        vec4<f32>(direction, startDistance));
}
