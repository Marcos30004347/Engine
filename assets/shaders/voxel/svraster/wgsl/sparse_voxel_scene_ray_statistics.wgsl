#include "sparse_voxel_scene_ray_types.wgsl"

struct SparseVoxelTree64Result {
    globalCell : vec3<i32>, status : u32,
    distance : f32, skippedDistance : f32, queryCount : u32, baseCellSteps : u32,
    highestLevel : u32, contentGeneration : u32, reserved0 : u32, reserved1 : u32,
}
const TREE64_RESULT_MISS : u32 = 1u;
const TREE64_RESULT_HIT : u32 = 2u;
const TREE64_RESULT_QUERY_LIMIT : u32 = 3u;
const TREE64_RESULT_UNKNOWN : u32 = 4u;
const TREE64_INVALID_REASON_INSTANCE_FLAGS : u32 = 1u;
const TREE64_INVALID_REASON_DYNAMIC_GENERATION : u32 = 2u;
const TREE64_INVALID_REASON_MODEL_INDEX : u32 = 3u;
const TREE64_INVALID_REASON_TERRAIN_STATE : u32 = 4u;
const TREE64_INVALID_REASON_TERRAIN_DESCRIPTOR_BOUNDS : u32 = 5u;
const TREE64_INVALID_REASON_TERRAIN_DESCRIPTOR_IDENTITY : u32 = 6u;
const TREE64_INVALID_REASON_TERRAIN_NODE_PAGE : u32 = 7u;
const TREE64_INVALID_REASON_TERRAIN_MATERIAL_PAGE : u32 = 8u;

struct SparseVoxelSceneRayStatisticsSettings {
    rayCount : u32,
    screenTraceEnabled : u32,
    reservoirEnabled : u32,
    lightCapacity : u32,
}

struct WorldGILight {
    positionRange : vec4<f32>,
    directionType : vec4<f32>,
    shapeParams : vec4<f32>,
    colorIntensity : vec4<f32>,
    flags : vec4<u32>,
}

@group(0) @binding(0) var<uniform> settings :
    SparseVoxelSceneRayStatisticsSettings;
@group(0) @binding(1) var<storage, read> rays :
    array<SparseVoxelSceneRay>;
@group(0) @binding(2) var<storage, read> screenHits :
    array<SparseVoxelSceneRayHit>;
@group(0) @binding(3) var<storage, read> mergedHits :
    array<SparseVoxelSceneRayHit>;
@group(0) @binding(4) var<storage, read> reservoirs :
    array<SparseVoxelDIReservoir>;
@group(0) @binding(5) var<storage, read> lights :
    array<WorldGILight>;
@group(0) @binding(6) var<storage, read> activeRayIndices :
    array<u32>;
@group(0) @binding(7) var<storage, read> activeRayState :
    array<u32>;
@group(0) @binding(8) var<storage, read_write> statistics :
    array<atomic<u32>>;
@group(0) @binding(9) var<storage, read> treeHits :
    array<SparseVoxelTree64Result>;
@group(0) @binding(10) var<storage, read> canonicalToCompact :
    array<u32>;

fn add_stat(index : u32, value : u32) {
    if (index < arrayLength(&statistics)) {
        atomicAdd(&statistics[index], value);
    }
}

@compute @workgroup_size(64, 1, 1)
fn collectSparseVoxelSceneRayStatistics(
    @builtin(global_invocation_id) gid : vec3<u32>) {
    let workIndex = gid.x;
    if (arrayLength(&activeRayState) < 4u ||
        workIndex >= activeRayState[3] ||
        workIndex >= arrayLength(&activeRayIndices)) {
        return;
    }
    let index = activeRayIndices[workIndex];
    if (index >= settings.rayCount ||
        index >= arrayLength(&rays) ||
        index >= arrayLength(&mergedHits)) {
        return;
    }

    let ray = rays[index];
    add_stat(0u, 1u);
    let rayActive =
        (ray.seedFlags.y & SPARSE_VOXEL_SCENE_RAY_FLAG_ACTIVE) != 0u;
    if (!rayActive) {
        add_stat(10u, 1u);
        return;
    }

    if (settings.screenTraceEnabled != 0u &&
        index < arrayLength(&screenHits)) {
        let screen = screenHits[index];
        if (screen.status == SPARSE_VOXEL_SCENE_RAY_STATUS_HIT) {
            add_stat(1u, 1u);
        } else {
            add_stat(2u, 1u);
        }

        // Diagnostic builds can retain the Tree64 trace for a ray already
        // answered by screen space. The canonical/compact map makes this a
        // comparison of the exact same receiver/light ray rather than two
        // aggregate frame counts.
        if (index < arrayLength(&canonicalToCompact)) {
            let compactIndex = canonicalToCompact[index];
            if (compactIndex != 0xffffffffu &&
                compactIndex < arrayLength(&treeHits)) {
                let treeStatus = treeHits[compactIndex].status;
                if (screen.status == SPARSE_VOXEL_SCENE_RAY_STATUS_HIT) {
                    if (treeStatus == TREE64_RESULT_HIT) {
                        add_stat(16u, 1u);
                    } else if (treeStatus == TREE64_RESULT_MISS) {
                        add_stat(17u, 1u);
                    } else if (treeStatus == 0u ||
                               treeStatus == TREE64_RESULT_QUERY_LIMIT ||
                               treeStatus == TREE64_RESULT_UNKNOWN) {
                        add_stat(18u, 1u);
                    }
                } else if (treeStatus == TREE64_RESULT_HIT) {
                    add_stat(19u, 1u);
                }
            }
        }
    }

    let merged = mergedHits[index];
    if (merged.identity.z == SPARSE_VOXEL_SCENE_RAY_BACKEND_TREE64 &&
        merged.status == SPARSE_VOXEL_SCENE_RAY_STATUS_HIT) {
        add_stat(3u, 1u);
    }
    if (merged.identity.z == SPARSE_VOXEL_SCENE_RAY_BACKEND_TREE64 &&
        merged.status != SPARSE_VOXEL_SCENE_RAY_STATUS_HIT) {
        add_stat(4u, 1u);
    }
    if (merged.status == SPARSE_VOXEL_SCENE_RAY_STATUS_ESCAPED_SCENE) {
        add_stat(5u, 1u);
    }
    if (merged.status == SPARSE_VOXEL_SCENE_RAY_STATUS_ESCAPED_TREE_VOLUME ||
        merged.status == SPARSE_VOXEL_SCENE_RAY_STATUS_UNKNOWN_RESIDENCY ||
        merged.status == SPARSE_VOXEL_SCENE_RAY_STATUS_QUERY_LIMIT) {
        add_stat(6u, 1u);
    }
    if (settings.reservoirEnabled != 0u &&
        index < arrayLength(&reservoirs)) {
        let reservoir = reservoirs[index];
        add_stat(7u, reservoir.identity.z);
        if (reservoir.state.y != 0u) {
            add_stat(8u, 1u);
        }
        if (reservoir.state.z == 2u) {
            add_stat(9u, 1u);
        }
    }
    if (merged.status == SPARSE_VOXEL_SCENE_RAY_STATUS_ESCAPED_TREE_VOLUME) {
        add_stat(11u, 1u);
    }
    if (merged.status == SPARSE_VOXEL_SCENE_RAY_STATUS_UNKNOWN_RESIDENCY) {
        add_stat(12u, 1u);
        if (index < arrayLength(&canonicalToCompact)) {
            let compactIndex = canonicalToCompact[index];
            if (compactIndex != 0xffffffffu &&
                compactIndex < arrayLength(&treeHits)) {
                let tree = treeHits[compactIndex];
                if (tree.status == 0u) {
                    switch tree.reserved1 {
                        case TREE64_INVALID_REASON_INSTANCE_FLAGS: {
                            add_stat(20u, 1u);
                        }
                        case TREE64_INVALID_REASON_DYNAMIC_GENERATION: {
                            add_stat(21u, 1u);
                        }
                        case TREE64_INVALID_REASON_MODEL_INDEX: {
                            add_stat(22u, 1u);
                        }
                        case TREE64_INVALID_REASON_TERRAIN_STATE: {
                            add_stat(23u, 1u);
                        }
                        case TREE64_INVALID_REASON_TERRAIN_DESCRIPTOR_BOUNDS: {
                            add_stat(24u, 1u);
                        }
                        case TREE64_INVALID_REASON_TERRAIN_DESCRIPTOR_IDENTITY: {
                            add_stat(25u, 1u);
                        }
                        case TREE64_INVALID_REASON_TERRAIN_NODE_PAGE: {
                            add_stat(26u, 1u);
                        }
                        case TREE64_INVALID_REASON_TERRAIN_MATERIAL_PAGE: {
                            add_stat(27u, 1u);
                        }
                        default: {}
                    }
                }
            }
        }
    }
    if (merged.status == SPARSE_VOXEL_SCENE_RAY_STATUS_QUERY_LIMIT) {
        add_stat(13u, 1u);
    }
    let lightIndex = ray.pixelRayLight.w;
    if (lightIndex < settings.lightCapacity &&
        lightIndex < arrayLength(&lights) &&
        lights[lightIndex].flags.y == 8u) {
        add_stat(15u, 1u);
        if (merged.status == SPARSE_VOXEL_SCENE_RAY_STATUS_HIT) {
            add_stat(14u, 1u);
        }
    }
}
