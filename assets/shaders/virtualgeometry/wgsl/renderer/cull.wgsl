enable subgroups;

struct AABB {
    minPoint: vec3<f32>,
    maxPoint: vec3<f32>,
};

struct InstancesData {
    modelMatrix: mat4x4<f32>,
    hierarchyRoot: u32,
    aabb: AABB,
};

struct LODBounds {
   center: vec4<f32>,
   radius: f32,
   error: f32,
};

struct ClusterData {
  selfCenterX : f32,
  selfCenterY : f32,
  selfCenterZ : f32,
  selfRadius : f32,
  selfError : f32,

  parentCenterX : f32,
  parentCenterY : f32,
  parentCenterZ : f32,
  parentError : f32,
  parentRadius : f32,

  indicesCount : u32,
  indicesOffset : u32,
};

const FLAG_ENABLED_INDEX = 0u;
const FLAG_LOADED_INDEX = 1u;
const FLAG_IS_LEAF_INDEX = 2u;
const FLAG_PADDING_INDEX = 3u;

struct HierarchyNode {
  aabb_max_x: f32,
  aabb_max_y: f32,
  aabb_max_z: f32,
  aabb_min_x: f32,
  aabb_min_y: f32,
  aabb_min_z: f32,

  center_x: f32,
  center_y: f32,
  center_z: f32,
  radius: f32,
  
  min_lod_error: f32,
  max_parent_lod_error: f32,

  child_start: u32,
  child_count: u32,
  page_index: u32,
  num_pages: u32,
  
  flags: u32,
};

//***************************************
//                 Culling
//***************************************

const CLUSTER_HEAD = 0u; 
const CLUSTER_TAIL = 1u;
const HIERARCHY_HEAD_INDEX = 2u; 
const HIERARCHY_TAIL_INDEX = 3u;
const WORK_REMAINING_INDEX = 4u;
const CLUSTERS_TO_RENDER = 5u;

struct CullingUniforms {
    view: mat4x4<f32>,
    proj: mat4x4<f32>,
    viewPosition: vec4<f32>,
    viewport: vec2<u32>,
    error: f32,
    instances_count: u32,
    clusters_count: u32,
};

@group(0) @binding(0) var<uniform> cullingUniforms: CullingUniforms;
@group(0) @binding(1) var<storage, read_write> scratch: array<vec2<u32>>;
@group(0) @binding(2) var<storage, read_write> sync: array<atomic<u32>>;
@group(0) @binding(3) var<storage, read> hierarchyNodes: array<HierarchyNode>;
@group(0) @binding(4) var<storage, read_write> clustersToCull: array<vec2<u32>>;
@group(0) @binding(5) var<storage, read_write> clustersToRender: array<u32>;
@group(0) @binding(6) var<storage, read_write> debugCulling: array<u32>;
@group(0) @binding(7) var<storage, read> instances: array<InstancesData>;
@group(0) @binding(8) var<storage, read> clusters: array<ClusterData>;

// depends on workgroup size
var<workgroup> workgroupInstanceIndexSharedMemory : array<u32, 128u>; // workgroup
var<workgroup> workgroupHierarchyIndexSharedMemory : array<u32, 128u>; // workgroup
var<workgroup> workgroupClusterIndexSharedMemory : array<u32, 128u>; // workgroup

var<workgroup> workgroupWorkRemaining : u32;

@compute @workgroup_size(128, 1, 1)
fn initialize_persistent_hierarchical_culling(@builtin(global_invocation_id) id: vec3<u32>) {
    if id.x >= cullingUniforms.clusters_count {
        return;
    }

    clustersToCull[id.x].x = 0xFFFFFFFFu;
    clustersToCull[id.x].y = 0xFFFFFFFFu;

    if id.x >= cullingUniforms.instances_count {
        return;
    }

    if id.x == 0u {
        atomicStore(&sync[HIERARCHY_HEAD_INDEX], 0u);
        atomicStore(&sync[HIERARCHY_TAIL_INDEX], 0u);
        atomicStore(&sync[WORK_REMAINING_INDEX], cullingUniforms.instances_count);
        atomicStore(&sync[CLUSTER_TAIL], 0u);
        atomicStore(&sync[CLUSTER_HEAD], 0u);
        atomicStore(&sync[CLUSTERS_TO_RENDER], 0u);
    }

    enqueueNodes(instances[id.x].hierarchyRoot, 1u, id.x);
}

fn enqueueNodes(start: u32, count: u32, instanceIdx: u32) {
    let index = atomicAdd(&sync[HIERARCHY_HEAD_INDEX], count);

    for (var i = 0u; i < count; i += 1u) {
        scratch[index + i] = vec2<u32>(start + i, instanceIdx);
    }
}

fn enqueueClusters(start: u32, count: u32, instanceIdx: u32) {
    let index = atomicAdd(&sync[CLUSTER_HEAD], count);

    for (var i = 0u; i < count; i += 1u) {
        clustersToCull[index + i].x = start + i;
        clustersToCull[index + i].y = instanceIdx;
    }
}


fn dequeue(laneId: u32, laneCount: u32, localThreadId: u32) -> bool {
    let elected = subgroupElect();

    var index = 0xFFFFFFFFu;
    var count = 0u;

    if elected {
        var tail = atomicLoad(&sync[HIERARCHY_TAIL_INDEX]);
        var head = atomicLoad(&sync[HIERARCHY_HEAD_INDEX]);

        while tail < head {
            var tip = min(head, tail + laneCount);

            if atomicCompareExchangeWeak(&sync[HIERARCHY_TAIL_INDEX], tail, tip).exchanged {
                index = tail;
                count = tip - tail;
                break;
            }

            tail = atomicLoad(&sync[HIERARCHY_TAIL_INDEX]);
            head = atomicLoad(&sync[HIERARCHY_HEAD_INDEX]);
        }
    }

    workgroupBarrier();

    index = subgroupBroadcastFirst(index);
    count = subgroupBroadcastFirst(count);

    var isDequeuingClusters = false;

    if count == 0u {
        isDequeuingClusters = true;
    }

    if elected && isDequeuingClusters {
        var tail = atomicLoad(&sync[CLUSTER_TAIL]);
        var head = atomicLoad(&sync[CLUSTER_HEAD]);

        while tail < head {
            var tip = min(head, tail + laneCount);

            if atomicCompareExchangeWeak(&sync[CLUSTER_TAIL], tail, tip).exchanged {
                index = tail;
                count = tip - tail;
                break;
            }

            tail = atomicLoad(&sync[CLUSTER_TAIL]);
            head = atomicLoad(&sync[CLUSTER_HEAD]);
        }
    }

    index = subgroupBroadcastFirst(index);
    count = subgroupBroadcastFirst(count);

    if isDequeuingClusters {
        if laneId < count {
            while clustersToCull[(index + laneId)].x == 0xFFFFFFFFu || clustersToCull[(index + laneId)].y == 0xFFFFFFFFu {
                continue;
            }

            workgroupClusterIndexSharedMemory[localThreadId] = clustersToCull[(index + laneId)].x;
            workgroupInstanceIndexSharedMemory[localThreadId] = clustersToCull[(index + laneId)].y;

            clustersToCull[(index + laneId)].x = 0xFFFFFFFFu;
            clustersToCull[(index + laneId)].y = 0xFFFFFFFFu;
        }

        return false;
    }


    if laneId < count {
        while scratch[(index + laneId)].x == 0xFFFFFFFFu || scratch[(index + laneId)].y == 0xFFFFFFFFu {
            continue;
        }

        workgroupHierarchyIndexSharedMemory[localThreadId] = scratch[(index + laneId)].x;
        workgroupInstanceIndexSharedMemory[localThreadId] = scratch[(index + laneId)].y;

        scratch[(index + laneId)].x = 0xFFFFFFFFu;
        scratch[(index + laneId)].y = 0xFFFFFFFFu;
    }

    return true;
}

fn get_projected_error(center: vec4<f32>, radius: f32, error: f32, model: mat4x4<f32>, proj: mat4x4<f32>, scale: f32) -> f32 {
    let sphere_world_space = (model * center);
    let radius_world_space = scale * radius;
    let error_world_space = scale * error;

    var projected_error = error_world_space;

    if cullingUniforms.proj[3][3] != 1.0 {
        let distance_to_closest_point_on_sphere = distance(sphere_world_space, cullingUniforms.viewPosition) - radius_world_space;
        let distance_to_closest_point_on_sphere_clamped_to_znear = max(distance_to_closest_point_on_sphere, cullingUniforms.proj[3][2]);
        projected_error /= distance_to_closest_point_on_sphere_clamped_to_znear;
    }

    projected_error *= cullingUniforms.view[1][1] * 0.5;
    projected_error *= f32(max(cullingUniforms.viewport[0], cullingUniforms.viewport[1]));

    return projected_error;
}

@compute @workgroup_size(128, 1, 1)
fn persistent_hierarchical_culling(
    @builtin(global_invocation_id) id: vec3<u32>,
    @builtin(workgroup_id) wid: vec3<u32>,
    @builtin(local_invocation_id) lid: vec3<u32>,
    @builtin(subgroup_invocation_id) laneid: u32,
    @builtin(subgroup_size) lane_count: u32
) {
    if lid.x == 0u {
        workgroupWorkRemaining = atomicLoad(&sync[WORK_REMAINING_INDEX]);
    }

    workgroupBarrier();

    while workgroupUniformLoad(&workgroupWorkRemaining) != 0u {

        workgroupHierarchyIndexSharedMemory[lid.x] = 0xFFFFFFFFu;
        workgroupInstanceIndexSharedMemory[lid.x] = 0xFFFFFFFFu;
        workgroupClusterIndexSharedMemory[lid.x] = 0xFFFFFFFFu;

        workgroupBarrier();

        let isProcessingHierarchy = dequeue(laneid, lane_count, lid.x);

        workgroupBarrier();

        var hierarchyNodeIndex = workgroupHierarchyIndexSharedMemory[lid.x];
        var instanceIndex = workgroupInstanceIndexSharedMemory[lid.x];
        var clusterIndex = workgroupClusterIndexSharedMemory[lid.x];

        if isProcessingHierarchy == false && clusterIndex != 0xFFFFFFFFu {
            debugCulling[id.x * 8u + 1u] += 1u;//1u;//hierarchyNode.child_count;

            let instance = instances[instanceIndex];
            let cluster = clusters[clusterIndex];

            let scale = max(length(instance.modelMatrix[0]), max(length(instance.modelMatrix[1]), length(instance.modelMatrix[2])));
            let selfCenter = vec4<f32>(cluster.selfCenterX, cluster.selfCenterY, cluster.selfCenterZ, 1.0);
            let parentCenter = vec4<f32>(cluster.parentCenterX, cluster.parentCenterY, cluster.parentCenterZ, 1.0);

            let parentError = get_projected_error(parentCenter, cluster.parentRadius, cluster.parentError, instance.modelMatrix, cullingUniforms.proj, scale);
            let selfError = get_projected_error(selfCenter, cluster.selfRadius, cluster.selfError, instance.modelMatrix, cullingUniforms.proj, scale);
                    
            let treshhold = scale * cullingUniforms.error;

            if selfError < treshhold && parentError >= treshhold {
                let index = atomicAdd(&sync[CLUSTERS_TO_RENDER], 1u);
                clustersToRender[index * 5u + 0u] = cluster.indicesCount;
                clustersToRender[index * 5u + 1u] = 1u;
                clustersToRender[index * 5u + 2u] = cluster.indicesOffset;
                clustersToRender[index * 5u + 3u] = 0u;
                clustersToRender[index * 5u + 4u] = clusterIndex; // TODO: replace with instance index
            }

            atomicSub(&sync[WORK_REMAINING_INDEX], 1u);
        }

        if isProcessingHierarchy == true && hierarchyNodeIndex != 0xFFFFFFFFu && instanceIndex != 0xFFFFFFFFu {
            debugCulling[id.x * 8u + 0u] += 1u;

            let instance = instances[instanceIndex];
            let hierarchyNode = hierarchyNodes[hierarchyNodeIndex];

            let scale = max(length(instance.modelMatrix[0]), max(length(instance.modelMatrix[1]), length(instance.modelMatrix[2])));
            let center = vec4<f32>(hierarchyNode.center_x, hierarchyNode.center_y, hierarchyNode.center_z, 1.0);

            let max_error = get_projected_error(center, hierarchyNode.radius, hierarchyNode.max_parent_lod_error, instance.modelMatrix, cullingUniforms.proj, scale);
            // let min_error = get_projected_error(center, hierarchyNode.radius, hierarchyNode.min_lod_error, instance.modelMatrix, cullingUniforms.proj, scale);

            let treshhold = scale * cullingUniforms.error;

            if max_error >= treshhold {
                atomicAdd(&sync[WORK_REMAINING_INDEX], hierarchyNode.child_count);

                if (hierarchyNode.flags & 1u) != 0u {
                    enqueueClusters(hierarchyNode.child_start, hierarchyNode.child_count, instanceIndex);
                } else {
                    enqueueNodes(hierarchyNodes[hierarchyNodeIndex].child_start, hierarchyNodes[hierarchyNodeIndex].child_count, instanceIndex);
                }
            }

            atomicSub(&sync[WORK_REMAINING_INDEX], 1u);
        }

        workgroupBarrier();

        if lid.x == 0u {
            workgroupWorkRemaining = atomicLoad(&sync[WORK_REMAINING_INDEX]);
        }

        workgroupBarrier();
    }
}
