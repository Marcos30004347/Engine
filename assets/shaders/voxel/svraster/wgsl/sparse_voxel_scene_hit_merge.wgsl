#include "sparse_voxel_tree64_common.wgsl"
#include "sparse_voxel_scene_ray_types.wgsl"

struct SparseVoxelTree64Result {
    globalCell : vec3<i32>, status : u32,
    distance : f32, skippedDistance : f32, queryCount : u32, baseCellSteps : u32,
    highestLevel : u32, contentGeneration : u32, reserved0 : u32, reserved1 : u32,
}
struct SparseVoxelSceneHitMergeSettings {
    rayCount : u32, screenTraceEnabled : u32, reserved1 : u32, reserved2 : u32,
    sceneMinimum : vec4<f32>,
    sceneMaximum : vec4<f32>,
}

struct SparseVoxelTree64Ray {
    originMaximumDistance : vec4<f32>,
    // w carries how far the world ray was advanced past the surface origin.
    direction : vec4<f32>,
}
const TREE64_RESULT_MISS : u32 = 1u;
const TREE64_RESULT_HIT : u32 = 2u;
const TREE64_RESULT_QUERY_LIMIT : u32 = 3u;
const TREE64_RESULT_UNKNOWN : u32 = 4u;

fn scene_tree64_start_distance(metadata : f32) -> f32 {
    if (metadata >= 0.0) { return metadata; }
    let packed = max(-metadata - 1.0, 0.0);
    let face = floor(packed / 128.0);
    return max(packed - face * 128.0, 0.0);
}

@group(0) @binding(0) var<uniform> settings : SparseVoxelSceneHitMergeSettings;
@group(0) @binding(1) var<storage, read> treeState : SparseVoxelTree64State;
@group(0) @binding(2) var<storage, read> rays : array<SparseVoxelSceneRay>;
@group(0) @binding(3) var<storage, read> screenHits : array<SparseVoxelSceneRayHit>;
@group(0) @binding(4) var<storage, read> treeHits : array<SparseVoxelTree64Result>;
@group(0) @binding(5) var<storage, read> canonicalToCompact : array<u32>;
@group(0) @binding(6) var<storage, read_write> mergedHits : array<SparseVoxelSceneRayHit>;
@group(0) @binding(7) var<storage, read> treeRays : array<SparseVoxelTree64Ray>;
@group(0) @binding(8) var<storage, read> activeRayIndices : array<u32>;
@group(0) @binding(9) var<storage, read> activeRayState : array<u32>;

fn point_inside_bounds(point : vec3<f32>, minimum : vec3<f32>, maximum : vec3<f32>) -> bool {
    return all(point >= minimum) && all(point <= maximum);
}

fn point_inside_half_open_bounds(point : vec3<f32>, minimum : vec3<f32>, maximum : vec3<f32>) -> bool {
    return all(point >= minimum) && all(point < maximum);
}

fn ray_fully_covered_by_tree(ray : SparseVoxelSceneRay) -> bool {
    let minimum = vec3<f32>(treeState.originCell) * treeState.cellSize;
    let maximum = (vec3<f32>(treeState.originCell) + vec3<f32>(treeState.logicalCellResolution)) * treeState.cellSize;
    let endpoint = ray.originMinimumDistance.xyz +
        ray.directionMaximumDistance.xyz * ray.directionMaximumDistance.w;
    return point_inside_half_open_bounds(ray.originMinimumDistance.xyz, minimum, maximum) &&
        point_inside_half_open_bounds(endpoint, minimum, maximum);
}

// Slab intersection of the ray with a box, clamped to [0, tMax].
struct RaySpan {
    entry : f32,
    exit : f32,
    valid : bool,
}

fn ray_slab_span(
    origin : vec3<f32>, direction : vec3<f32>,
    minimum : vec3<f32>, maximum : vec3<f32>, tMax : f32) -> RaySpan {
    var entry = 0.0;
    var exit = tMax;
    for (var axis = 0u; axis < 3u; axis += 1u) {
        let step = direction[axis];
        if (abs(step) < 1.0e-12) {
            // Parallel to this slab: either always inside it or never.
            if (origin[axis] < minimum[axis] || origin[axis] > maximum[axis]) {
                return RaySpan(0.0, 0.0, false);
            }
            continue;
        }
        let inverse = 1.0 / step;
        let near = (minimum[axis] - origin[axis]) * inverse;
        let far = (maximum[axis] - origin[axis]) * inverse;
        entry = max(entry, min(near, far));
        exit = min(exit, max(near, far));
    }
    return RaySpan(entry, exit, exit >= entry);
}

// Did the tree actually trace every part of this ray that could contain an
// occluder?
//
// Only the span inside the scene bounds can hold geometry; past it the ray is
// in empty space by definition, so the tree does not have to cover it. That
// distinction is the whole point: a sky ray's tmax is the scene diagonal or
// more, so its endpoint is essentially always outside both the scene and the
// clipmap. Asking for the whole ray to be covered would mark every sky ray
// unknown and the scene would never light; asking for none of it - which is
// what testing `ray_exits_scene` first amounted to - reports a ray the tree
// never finished as "reached the sky", which is a missing shadow.
fn ray_scene_span_covered_by_tree(ray : SparseVoxelSceneRay) -> bool {
    let origin = ray.originMinimumDistance.xyz;
    let direction = ray.directionMaximumDistance.xyz;
    let tMax = ray.directionMaximumDistance.w;
    let scene = ray_slab_span(
        origin, direction,
        settings.sceneMinimum.xyz, settings.sceneMaximum.xyz, tMax);
    // Never enters the scene at all: nothing there to occlude it.
    if (!scene.valid) { return true; }
    let treeMinimum = vec3<f32>(treeState.originCell) * treeState.cellSize;
    let treeMaximum =
        (vec3<f32>(treeState.originCell) +
         vec3<f32>(treeState.logicalCellResolution)) * treeState.cellSize;
    let tree = ray_slab_span(
        origin, direction, treeMinimum, treeMaximum, tMax);
    if (!tree.valid) { return false; }
    return tree.entry <= scene.entry && scene.exit <= tree.exit;
}

fn ray_exits_scene(ray : SparseVoxelSceneRay) -> bool {
    let endpoint = ray.originMinimumDistance.xyz +
        ray.directionMaximumDistance.xyz * ray.directionMaximumDistance.w;
    return !point_inside_bounds(
            ray.originMinimumDistance.xyz, settings.sceneMinimum.xyz, settings.sceneMaximum.xyz) ||
        !point_inside_bounds(endpoint, settings.sceneMinimum.xyz, settings.sceneMaximum.xyz);
}

@compute @workgroup_size(64, 1, 1)
fn mergeSparseVoxelSceneHits(@builtin(global_invocation_id) gid : vec3<u32>) {
    let workIndex = gid.x;
    if (arrayLength(&activeRayState) < 4u ||
        workIndex >= activeRayState[3] ||
        workIndex >= arrayLength(&activeRayIndices)) { return; }
    let index = activeRayIndices[workIndex];
    if (index >= settings.rayCount || index >= arrayLength(&rays) ||
        index >= arrayLength(&treeHits) || index >= arrayLength(&mergedHits) ||
        (settings.screenTraceEnabled != 0u &&
         index >= arrayLength(&screenHits))) { return; }
    let ray = rays[index];
    if (settings.screenTraceEnabled != 0u) {
        let screen = screenHits[index];
        if (screen.status == SPARSE_VOXEL_SCENE_RAY_STATUS_HIT &&
            settings.reserved1 == 0u) {
            mergedHits[index] = screen;
            return;
        }
    }
    let compactIndex = canonicalToCompact[index];
    if (compactIndex == 0xffffffffu || compactIndex >= arrayLength(&treeHits)) {
        mergedHits[index] = sparse_voxel_invalid_scene_hit(ray, SPARSE_VOXEL_SCENE_RAY_STATUS_UNKNOWN_RESIDENCY, SPARSE_VOXEL_SCENE_RAY_BACKEND_TREE64); return;
    }
    let tree = treeHits[compactIndex];
    if (tree.status == TREE64_RESULT_HIT) {
        // Screen-space shadows are an accelerator/near-field refinement, not
        // a second visibility definition. At coplanar edges its thickness
        // test can report an isolated hit that the authored Tree64 geometry
        // does not contain. When confirmation is enabled, preserve the screen
        // hit only after the world backend agrees that this ray is occluded.
        if (settings.screenTraceEnabled != 0u &&
            settings.reserved1 != 0u &&
            screenHits[index].status == SPARSE_VOXEL_SCENE_RAY_STATUS_HIT) {
            mergedHits[index] = screenHits[index];
            return;
        }
        // The world ray may have been advanced past the receiver's own cell
        // and past the span the screen tracer already covered, so the
        // traversal reports a distance relative to that advanced origin. Add
        // the offset back: hit distance is matched against history in
        // can_reuse, and a frame-varying origin would desync that comparison
        // and reset accumulation every frame.
        // treeRays is the compacted buffer, so it is indexed by compactIndex.
        let startDistance = select(
            0.0,
            scene_tree64_start_distance(
                treeRays[compactIndex].direction.w),
            compactIndex < arrayLength(&treeRays));
        let absoluteDistance = tree.distance + startDistance;
        let hitPosition = ray.originMinimumDistance.xyz +
            ray.directionMaximumDistance.xyz * absoluteDistance;
        mergedHits[index] = SparseVoxelSceneRayHit(vec4<f32>(hitPosition, absoluteDistance), tree.globalCell, SPARSE_VOXEL_SCENE_RAY_STATUS_HIT,
            vec4<u32>(ray.seedFlags.x, ray.pixelRayLight.w, SPARSE_VOXEL_SCENE_RAY_BACKEND_TREE64, ray.seedFlags.z)); return;
    }
    if (tree.status == TREE64_RESULT_QUERY_LIMIT) {
        mergedHits[index] = sparse_voxel_invalid_scene_hit(ray, SPARSE_VOXEL_SCENE_RAY_STATUS_QUERY_LIMIT, SPARSE_VOXEL_SCENE_RAY_BACKEND_TREE64); return;
    }
    if (tree.status == 0u || tree.status == TREE64_RESULT_UNKNOWN) {
        mergedHits[index] = sparse_voxel_invalid_scene_hit(ray, SPARSE_VOXEL_SCENE_RAY_STATUS_UNKNOWN_RESIDENCY, SPARSE_VOXEL_SCENE_RAY_BACKEND_TREE64); return;
    }
    // Coverage is decided first. A miss only means "reached the sky" if the
    // tree actually traced everything that could have stopped the ray; if it
    // did not, the answer is unknown and consumers must keep history rather
    // than light the pixel. The old order tested `ray_exits_scene` first,
    // which every sky ray passes by construction, so an untraced ray was
    // reported as lit and `ESCAPED_TREE_VOLUME` was unreachable - measured at
    // exactly 0 on Sponza while 93% of rays took `ESCAPED_SCENE`.
    var status = SPARSE_VOXEL_SCENE_RAY_STATUS_MISS_TMAX;
    if (!ray_scene_span_covered_by_tree(ray)) {
        status = SPARSE_VOXEL_SCENE_RAY_STATUS_ESCAPED_TREE_VOLUME;
    } else if (ray_exits_scene(ray)) {
        status = SPARSE_VOXEL_SCENE_RAY_STATUS_ESCAPED_SCENE;
    }
    mergedHits[index] = sparse_voxel_invalid_scene_hit(ray, status, SPARSE_VOXEL_SCENE_RAY_BACKEND_TREE64);
}
