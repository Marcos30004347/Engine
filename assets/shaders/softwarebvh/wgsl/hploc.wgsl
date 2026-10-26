#ifdef WGSL_ENABLE_SUBGROUPS
enable subgroups;
#endif

// Standalone HPLOC prototype over precomputed primitive AABBs.
// Expected host flow:
// 1. Dispatch hploc_reset.
// 2. Dispatch hploc_encode_morton.
// 3. Sort mortonCodes + clusterRefsIn externally.
// 4. Dispatch hploc_merge_pass repeatedly, ping-ponging clusterRefsIn/clusterRefsOut
//    and updating uniforms.activeClusterCount until buildSignals[SIGNAL_OUTPUT_COUNT] == 1.

@diagnostic(off, subgroup_uniformity)
fn unsafeSubgroupExclusiveAdd(x: u32) -> u32 { return subgroupExclusiveAdd(x); }

@diagnostic(off, subgroup_uniformity)
fn unsafeSubgroupAdd(x: u32) -> u32 { return subgroupAdd(x); }

@diagnostic(off, subgroup_uniformity)
fn unsafeSubgroupShuffleU32(x: u32, source: u32) -> u32 { return subgroupShuffle(x, source); }

@diagnostic(off, subgroup_uniformity)
fn unsafeSubgroupShuffleF32(x: f32, source: u32) -> f32 { return subgroupShuffle(x, source); }

const INVALID_INDEX = 0xffffffffu;
const CLUSTER_KIND_LEAF = 0u;
const CLUSTER_KIND_INTERNAL = 1u;
const STATUS_OK = 0u;
const STATUS_ERR_CAPACITY = 1u;
const STATUS_ERR_SUBGROUP_LAYOUT = 2u;
const WORKGROUP_SIZE = 64u;
const DEFAULT_SEARCH_RADIUS = 8u;
const SIGNAL_BVH2_ALLOCATOR = 0u;
const SIGNAL_OUTPUT_COUNT = 1u;
const SIGNAL_STATUS = 2u;
const SIGNAL_COUNT = 3u;
const F32_MAX = 3.402823466e+38;

struct PrimitiveAabbGPU {
    minPoint: vec3<f32>,
    _pad0: u32,
    maxPoint: vec3<f32>,
    _pad1: u32,
};

struct ClusterRefGPU {
    index: u32,
    kind: u32,
};

struct BVH2NodeGPU {
    left: vec2<u32>,
    right: vec2<u32>,
    minPoint: vec3<f32>,
    _pad0: u32,
    maxPoint: vec3<f32>,
    _pad1: u32,
};

struct Uniforms {
    primitiveCount: u32,
    activeClusterCount: u32,
    maxBvh2Nodes: u32,
    searchRadius: u32,
    sceneMin: vec3<f32>,
    _pad0: u32,
    sceneMax: vec3<f32>,
    _pad1: u32,
};

struct AabbGPU {
    minPoint: vec3<f32>,
    maxPoint: vec3<f32>,
};

@group(0) @binding(0) var<uniform> uniforms: Uniforms;
@group(0) @binding(1) var<storage, read> primitiveAabbs: array<PrimitiveAabbGPU>;
@group(0) @binding(2) var<storage, read_write> mortonCodes: array<u32>;
@group(0) @binding(3) var<storage, read_write> clusterRefsIn: array<ClusterRefGPU>;
@group(0) @binding(4) var<storage, read_write> clusterRefsOut: array<ClusterRefGPU>;
@group(0) @binding(5) var<storage, read_write> bvh2Nodes: array<BVH2NodeGPU>;
@group(0) @binding(6) var<storage, read_write> buildSignals: array<atomic<u32>>;
@group(0) @binding(7) var<storage, read_write> buildDebug: array<atomic<u32>>;

var<workgroup> wgClusterIndex: array<u32, WORKGROUP_SIZE>;
var<workgroup> wgClusterKind: array<u32, WORKGROUP_SIZE>;
var<workgroup> wgBoundsMin: array<vec4<f32>, WORKGROUP_SIZE>;
var<workgroup> wgBoundsMax: array<vec4<f32>, WORKGROUP_SIZE>;

fn makeInvalidAabb() -> AabbGPU {
    return AabbGPU(vec3<f32>(F32_MAX), vec3<f32>(-F32_MAX));
}

fn isValidAabb(bounds: AabbGPU) -> bool {
    return all(bounds.minPoint <= bounds.maxPoint);
}

fn unionAabb(a: AabbGPU, b: AabbGPU) -> AabbGPU {
    if !isValidAabb(a) {
        return b;
    }
    if !isValidAabb(b) {
        return a;
    }
    return AabbGPU(min(a.minPoint, b.minPoint), max(a.maxPoint, b.maxPoint));
}

fn surfaceArea(bounds: AabbGPU) -> f32 {
    let d = max(bounds.maxPoint - bounds.minPoint, vec3<f32>(0.0));
    return 2.0 * (d.x * d.y + d.x * d.z + d.y * d.z);
}

fn unionSurfaceArea(a: AabbGPU, b: AabbGPU) -> f32 {
    return surfaceArea(unionAabb(a, b));
}

fn loadPrimitiveBounds(index: u32) -> AabbGPU {
    let src = primitiveAabbs[index];
    return AabbGPU(src.minPoint, src.maxPoint);
}

fn loadClusterBounds(cluster: ClusterRefGPU) -> AabbGPU {
    if cluster.kind == CLUSTER_KIND_LEAF {
        if cluster.index >= uniforms.primitiveCount {
            return makeInvalidAabb();
        }
        return loadPrimitiveBounds(cluster.index);
    }

    if cluster.index >= uniforms.maxBvh2Nodes {
        return makeInvalidAabb();
    }

    let node = bvh2Nodes[cluster.index];
    return AabbGPU(node.minPoint, node.maxPoint);
}

fn clusterCenter(bounds: AabbGPU) -> vec3<f32> {
    return 0.5 * (bounds.minPoint + bounds.maxPoint);
}

fn expandBits(value: u32) -> u32 {
    var v = value & 0x3ffu;
    v = (v | (v << 16u)) & 0x030000ffu;
    v = (v | (v << 8u)) & 0x0300f00fu;
    v = (v | (v << 4u)) & 0x030c30c3u;
    v = (v | (v << 2u)) & 0x09249249u;
    return v;
}

fn morton3(normalizedPosition: vec3<f32>) -> u32 {
    let p = clamp(normalizedPosition, vec3<f32>(0.0), vec3<f32>(0.999999));
    let x = u32(clamp(p.x * 1024.0, 0.0, 1023.0));
    let y = u32(clamp(p.y * 1024.0, 0.0, 1023.0));
    let z = u32(clamp(p.z * 1024.0, 0.0, 1023.0));
    return (expandBits(x) << 2u) | (expandBits(y) << 1u) | expandBits(z);
}

fn encodeMorton(bounds: AabbGPU) -> u32 {
    if !isValidAabb(bounds) {
        return INVALID_INDEX;
    }

    let sceneExtent = max(uniforms.sceneMax - uniforms.sceneMin, vec3<f32>(1.0e-8));
    let center = clusterCenter(bounds);
    let normalized = (center - uniforms.sceneMin) / sceneExtent;
    return morton3(normalized);
}

fn shuffleClusterRef(cluster: ClusterRefGPU, source: u32) -> ClusterRefGPU {
    return ClusterRefGPU(
        unsafeSubgroupShuffleU32(cluster.index, source),
        unsafeSubgroupShuffleU32(cluster.kind, source));
}

fn shuffleAabb(bounds: AabbGPU, source: u32) -> AabbGPU {
    let minPoint = vec3<f32>(
        unsafeSubgroupShuffleF32(bounds.minPoint.x, source),
        unsafeSubgroupShuffleF32(bounds.minPoint.y, source),
        unsafeSubgroupShuffleF32(bounds.minPoint.z, source));
    let maxPoint = vec3<f32>(
        unsafeSubgroupShuffleF32(bounds.maxPoint.x, source),
        unsafeSubgroupShuffleF32(bounds.maxPoint.y, source),
        unsafeSubgroupShuffleF32(bounds.maxPoint.z, source));
    return AabbGPU(minPoint, maxPoint);
}

fn findNearestNeighbor(bounds: AabbGPU, activeCount: u32, lane: u32, radius: u32) -> u32 {
    if lane >= activeCount || !isValidAabb(bounds) {
        return INVALID_INDEX;
    }

    var bestNeighbor = INVALID_INDEX;
    var bestCost = F32_MAX;
    let clampedRadius = min(radius, activeCount - 1u);

    for (var offset = 1u; offset <= clampedRadius; offset += 1u) {
        if lane >= offset {
            let neighborLane = lane - offset;
            let neighborBounds = shuffleAabb(bounds, neighborLane);
            let cost = unionSurfaceArea(bounds, neighborBounds);
            if cost < bestCost {
                bestCost = cost;
                bestNeighbor = neighborLane;
            }
        }

        if lane + offset < activeCount {
            let neighborLane = lane + offset;
            let neighborBounds = shuffleAabb(bounds, neighborLane);
            let cost = unionSurfaceArea(bounds, neighborBounds);
            if cost < bestCost {
                bestCost = cost;
                bestNeighbor = neighborLane;
            }
        }
    }

    return bestNeighbor;
}

fn storeCompactCluster(dst: u32, cluster: ClusterRefGPU, bounds: AabbGPU) {
    wgClusterIndex[dst] = cluster.index;
    wgClusterKind[dst] = cluster.kind;
    wgBoundsMin[dst] = vec4<f32>(bounds.minPoint, 0.0);
    wgBoundsMax[dst] = vec4<f32>(bounds.maxPoint, 0.0);
}

fn loadCompactCluster(src: u32, cluster: ptr<function, ClusterRefGPU>, bounds: ptr<function, AabbGPU>) {
    (*cluster) = ClusterRefGPU(wgClusterIndex[src], wgClusterKind[src]);
    (*bounds) = AabbGPU(wgBoundsMin[src].xyz, wgBoundsMax[src].xyz);
}

@compute @workgroup_size(WORKGROUP_SIZE, 1, 1)
fn hploc_reset(@builtin(local_invocation_id) localId: vec3<u32>) {
    if localId.x < SIGNAL_COUNT {
        atomicStore(&buildSignals[localId.x], 0u);
    }

    if localId.x == SIGNAL_STATUS {
        atomicStore(&buildSignals[SIGNAL_STATUS], STATUS_OK);
    }
}

@compute @workgroup_size(WORKGROUP_SIZE, 1, 1)
fn hploc_encode_morton(@builtin(global_invocation_id) globalId: vec3<u32>) {
    let primitiveIndex = globalId.x;
    if primitiveIndex >= uniforms.primitiveCount {
        return;
    }

    let bounds = loadPrimitiveBounds(primitiveIndex);
    mortonCodes[primitiveIndex] = encodeMorton(bounds);
    clusterRefsIn[primitiveIndex] = ClusterRefGPU(primitiveIndex, CLUSTER_KIND_LEAF);
}

@compute @workgroup_size(WORKGROUP_SIZE, 1, 1)
fn hploc_merge_pass(
    @builtin(workgroup_id) workgroupId: vec3<u32>,
    @builtin(local_invocation_id) localId: vec3<u32>,
    @builtin(subgroup_invocation_id) subgroupLane: u32,
    @builtin(subgroup_size) laneCount: u32) {
    if laneCount == 0u || (WORKGROUP_SIZE % laneCount) != 0u {
        if localId.x == 0u {
            atomicStore(&buildSignals[SIGNAL_STATUS], STATUS_ERR_SUBGROUP_LAYOUT);
        }
        return;
    }

    let subgroupIndexInWorkgroup = localId.x / laneCount;
    let subgroupsPerWorkgroup = WORKGROUP_SIZE / laneCount;
    let globalSubgroupIndex = workgroupId.x * subgroupsPerWorkgroup + subgroupIndexInWorkgroup;
    let segmentStart = globalSubgroupIndex * laneCount;

    if segmentStart >= uniforms.activeClusterCount {
        return;
    }

    var activeCount = min(laneCount, uniforms.activeClusterCount - segmentStart);
    let subgroupBase = localId.x - subgroupLane;

    var currentCluster = ClusterRefGPU(INVALID_INDEX, CLUSTER_KIND_LEAF);
    var currentBounds = makeInvalidAabb();

    if subgroupLane < activeCount {
        currentCluster = clusterRefsIn[segmentStart + subgroupLane];
        currentBounds = loadClusterBounds(currentCluster);
    }

    workgroupBarrier();

    let searchRadius = select(DEFAULT_SEARCH_RADIUS, uniforms.searchRadius, uniforms.searchRadius != 0u);

    loop {
        if activeCount <= 1u {
            break;
        }

        var partnerLane = INVALID_INDEX;
        if subgroupLane < activeCount {
            partnerLane = findNearestNeighbor(currentBounds, activeCount, subgroupLane, searchRadius);
        }

        var reciprocalLane = INVALID_INDEX;
        if partnerLane != INVALID_INDEX {
            reciprocalLane = unsafeSubgroupShuffleU32(partnerLane, partnerLane);
        }

        let createsNode = subgroupLane < activeCount &&
            partnerLane != INVALID_INDEX &&
            reciprocalLane == subgroupLane &&
            subgroupLane < partnerLane;

        let removedByPair = subgroupLane < activeCount &&
            partnerLane != INVALID_INDEX &&
            reciprocalLane == subgroupLane &&
            partnerLane < subgroupLane;

        var nextCluster = currentCluster;
        var nextBounds = currentBounds;

        if createsNode {
            let rightCluster = shuffleClusterRef(currentCluster, partnerLane);
            let rightBounds = shuffleAabb(currentBounds, partnerLane);
            let mergedBounds = unionAabb(currentBounds, rightBounds);
            let nodeIndex = atomicAdd(&buildSignals[SIGNAL_BVH2_ALLOCATOR], 1u);

            if nodeIndex >= uniforms.maxBvh2Nodes {
                atomicStore(&buildSignals[SIGNAL_STATUS], STATUS_ERR_CAPACITY);
            } else {
                bvh2Nodes[nodeIndex].left = vec2<u32>(currentCluster.index, currentCluster.kind);
                bvh2Nodes[nodeIndex].right = vec2<u32>(rightCluster.index, rightCluster.kind);
                bvh2Nodes[nodeIndex].minPoint = mergedBounds.minPoint;
                bvh2Nodes[nodeIndex].maxPoint = mergedBounds.maxPoint;
                nextCluster = ClusterRefGPU(nodeIndex, CLUSTER_KIND_INTERNAL);
                nextBounds = mergedBounds;
            }
        }

        let keep = subgroupLane < activeCount && !removedByPair;
        let keepValue = select(0u, 1u, keep);
        let compactIndex = unsafeSubgroupExclusiveAdd(keepValue);
        let nextActiveCount = unsafeSubgroupAdd(keepValue);

        if keep {
            storeCompactCluster(subgroupBase + compactIndex, nextCluster, nextBounds);
        }

        workgroupBarrier();

        if subgroupLane < nextActiveCount {
            loadCompactCluster(subgroupBase + subgroupLane, &currentCluster, &currentBounds);
        } else {
            currentCluster = ClusterRefGPU(INVALID_INDEX, CLUSTER_KIND_LEAF);
            currentBounds = makeInvalidAabb();
        }

        activeCount = nextActiveCount;
        workgroupBarrier();
    }

    if subgroupLane == 0u && activeCount > 0u {
        let outputIndex = atomicAdd(&buildSignals[SIGNAL_OUTPUT_COUNT], 1u);
        clusterRefsOut[outputIndex] = currentCluster;
    }
}
