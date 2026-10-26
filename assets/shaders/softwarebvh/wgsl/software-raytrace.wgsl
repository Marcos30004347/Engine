const WORKGROUP_SIZE_X = 8u;
const WORKGROUP_SIZE_Y = 8u;
const INVALID_INDEX = 0xffffffffu;
const DEBUG_MODE_HEATMAP = 1u;
const DEBUG_MODE_RBVH_CARD_AABB = 16u;
const DEBUG_HIT_INSTANCE_RAY_GENERATED = 0xfffffffbu;
const DEBUG_HIT_INSTANCE_INVALID_RAY = 0xfffffffcu;
const DEBUG_HIT_INSTANCE_HEATMAP = 0xfffffffdu;
const DEBUG_HIT_INSTANCE_MARKER = 0xfffffffeu;
const DEBUG_HIT_TRACE_DISABLED = 0xfffffffeu;
const DEBUG_HIT_RBVH_CARD_AABB = 0xfffffffau;
const TLAS_NODE_DEPTH_SHIFT = 8u;
const OVERLAY_FLAG_TLAS_BOUNDS = 1u;
const OVERLAY_FLAG_TRACE_ENABLED = 2u;
const OVERLAY_FLAG_TLAS_CWBVH_BOUNDS = 4u;
const MAX_STACK_DEPTH = 16u;
const TLAS_TRAVERSAL_STACK_SIZE = MAX_STACK_DEPTH;
const RBVH_CARD_DDA_MAX_STEPS = 1024u;
const RBVH_CARD_DDA_MAX_MIP_LEVEL = 10u;
const RAY_SORT_BIN_COUNT = 256u;

#include "software-raytrace-ray.wgsl"

struct Uniforms {
    width: u32,
    height: u32,
    tlasNodeCount: u32,
    debugMode: u32,
    inverseViewProjection: mat4x4<f32>,
    cameraPosition: vec3<f32>,
    tanHalfFovY: f32,
    cameraForward: vec3<f32>,
    aspectRatio: f32,
    cameraRight: vec3<f32>,
    tMin: f32,
    cameraUp: vec3<f32>,
    tMax: f32,
    backgroundColor: vec4<f32>,
    heatmapMaxIterations: u32,
    rayCount: u32,
    overlayFlags: u32,
    rayDispatchWidth: u32,
    overlayColorAndThickness: vec4<f32>,
    traceControl: vec4<f32>,
    primitiveMode: u32,
    rbvhLodLevel: u32,
    rbvhDepthAtlasTextureWidth: u32,
    rbvhDepthAtlasTextureHeight: u32,
};

struct InstanceGPU {
    transform: mat4x4<f32>,
    inverseTransform: mat4x4<f32>,
    previousTransform: mat4x4<f32>,
    aabbMin: vec3<f32>,
    blasHandle: u32,
    aabbMax: vec3<f32>,
    sceneInstanceId: u32,
    flags: u32,
    _padding: array<u32, 3>,
};

struct InstanceAABBGPU {
    minPoint: vec3<f32>,
    instanceIndex: u32,
    maxPoint: vec3<f32>,
    blasHandle: u32,
};

struct BVH4NodeGPU {
    childMinX: vec4<f32>,
    childMinY: vec4<f32>,
    childMinZ: vec4<f32>,
    childIndex: vec4<u32>,
    childMaxX: vec4<f32>,
    childMaxY: vec4<f32>,
    childMaxZ: vec4<f32>,
    childMeta: vec4<u32>,
};

struct WorldInstanceRangeGPU {
    instanceIndex: u32,
    sceneInstanceId: u32,
    blasHandle: u32,
    flags: u32,
    worldCWBVHNodeOffset: u32,
    worldCWBVHNodeCount: u32,
    worldBVH8NodeOffset: u32,
    worldBVH8NodeCount: u32,
    worldTriangleOffset: u32,
    worldTriangleCount: u32,
    worldLeafRemapOffset: u32,
    worldLeafRemapCount: u32,
    valid: u32,
    dynamic: u32,
    needsUpdate: u32,
    primitiveRemapOffset: u32,
};

struct HitState {
    bestT: f32,
    bestPrimitive: u32,
    bestInstance: u32,
    bestBary: vec2<f32>,
    iterationCount: u32,
};

struct TraceStats {
    totalRays: atomic<u32>,
    activeRays: atomic<u32>,
    hitRays: atomic<u32>,
    missRays: atomic<u32>,
    invalidDirectionRays: atomic<u32>,
    tlasNodeVisits: atomic<u32>,
    blasNodeVisits: atomic<u32>,
    worldBlasNodeVisits: atomic<u32>,
    triangleTests: atomic<u32>,
    workgroupQueuePushes: atomic<u32>,
    workgroupQueuePops: atomic<u32>,
    localStackSpills: atomic<u32>,
    overlayNodeTests: atomic<u32>,
    overlayEdgeHits: atomic<u32>,
    lastHitPrimitive: atomic<u32>,
    lastHitInstance: atomic<u32>,
    lastHitValid: atomic<u32>,
    traversalEarlyOutSkips: atomic<u32>,
    debugPixelsReachedValidBLAS: atomic<u32>,
    debugPixelsReachedInvalidBLAS: atomic<u32>,
    debugPixelsVisitedBLASNode: atomic<u32>,
    debugPixelsReadTriangle: atomic<u32>,
    debugPixelsRejectedTriangle: atomic<u32>,
    debugPixelsAcceptedTriangle: atomic<u32>,
    debugPixelsRejectedWorldT: atomic<u32>,
    debugPixelsAcceptedCandidate: atomic<u32>,
    debugPixelsBLASLeafGroup: atomic<u32>,
    debugPixelsInvalidLeafRemap: atomic<u32>,
    debugPixelsInvalidLocalRay: atomic<u32>,
    dynamicRayFetchCursor: atomic<u32>,
};

@group(0) @binding(0) var<uniform> uniforms: Uniforms;
@group(0) @binding(1) var<storage, read> tlasInstances: array<InstanceGPU>;
@group(0) @binding(2) var<storage, read> tlasNodes: array<vec4<u32>>;
@group(0) @binding(3) var<storage, read> tlasLeafInstances: array<u32>;
@group(0) @binding(9) var<storage, read> blasNodes: array<vec4<u32>>;
#ifdef SWRT_WORLD_CWBVH
@group(0) @binding(10) var<storage, read> blasLeafRemap: array<vec2<u32>>;
#else
@group(0) @binding(10) var<storage, read> blasLeafRemap: array<u32>;
#endif
@group(0) @binding(11) var<storage, read_write> traceStats: TraceStats;
@group(0) @binding(12) var outputTexture: texture_storage_2d<rgba16float, write>;
@group(0) @binding(13) var<storage, read_write> rayInput: array<TraceRay>;
@group(0) @binding(14) var<storage, read_write> hitOutput: array<TraceHit>;
@group(0) @binding(19) var<storage, read> tlasInstanceAabbs: array<InstanceAABBGPU>;
@group(0) @binding(20) var<storage, read> tlasBvh4Nodes: array<BVH4NodeGPU>;
#ifdef SWRT_WORLD_CWBVH
@group(0) @binding(21) var<storage, read> worldInstanceRanges: array<WorldInstanceRangeGPU>;
#endif
@group(0) @binding(22) var<storage, read_write> raySortBinCounts: array<atomic<u32>>;
@group(0) @binding(23) var<storage, read_write> raySortBinOffsets: array<u32>;
@group(0) @binding(24) var<storage, read_write> raySortIds: array<u32>;

fn load_local_leaf_remap(index: u32) -> u32 {
#ifdef SWRT_WORLD_CWBVH
    return blasLeafRemap[index].x;
#else
    return blasLeafRemap[index];
#endif
}

fn trace_stat_add_local_stack_spill(value: u32) {
#ifdef SWRT_ENABLE_TRACE_STATS
    atomicAdd(&traceStats.localStackSpills, value);
#endif
}

fn trace_stat_add_tlas_node_visit(value: u32) {
#ifdef SWRT_ENABLE_TRACE_STATS
    atomicAdd(&traceStats.tlasNodeVisits, value);
#endif
}

fn trace_stat_add_blas_node_visit(value: u32) {
#ifdef SWRT_ENABLE_TRACE_STATS
    atomicAdd(&traceStats.blasNodeVisits, value);
#endif
}

fn trace_stat_add_world_blas_node_visit(value: u32) {
#ifdef SWRT_ENABLE_TRACE_STATS
    atomicAdd(&traceStats.blasNodeVisits, value);
    atomicAdd(&traceStats.worldBlasNodeVisits, value);
#endif
}

fn trace_stat_add_triangle_test(value: u32) {
#ifdef SWRT_ENABLE_TRACE_STATS
    atomicAdd(&traceStats.triangleTests, value);
#endif
}

fn trace_stat_begin_ray() {
#ifdef SWRT_ENABLE_TRACE_STATS
    atomicAdd(&traceStats.totalRays, 1u);
#endif
}

fn trace_stat_begin_valid_ray() {
#ifdef SWRT_ENABLE_TRACE_STATS
    atomicAdd(&traceStats.activeRays, 1u);
#endif
}

fn trace_stat_add_invalid_ray() {
#ifdef SWRT_ENABLE_TRACE_STATS
    atomicAdd(&traceStats.invalidDirectionRays, 1u);
    atomicAdd(&traceStats.missRays, 1u);
#endif
}

fn trace_stat_add_miss() {
#ifdef SWRT_ENABLE_TRACE_STATS
    atomicAdd(&traceStats.missRays, 1u);
#endif
}

fn trace_stat_add_hit(primitiveId: u32, instanceId: u32) {
#ifdef SWRT_ENABLE_TRACE_STATS
    atomicAdd(&traceStats.hitRays, 1u);
    atomicStore(&traceStats.lastHitPrimitive, primitiveId);
    atomicStore(&traceStats.lastHitInstance, instanceId);
    atomicStore(&traceStats.lastHitValid, 1u);
#endif
}

@compute @workgroup_size(1, 1, 1)
fn clear_stats() {
    atomicStore(&traceStats.totalRays, 0u);
    atomicStore(&traceStats.activeRays, 0u);
    atomicStore(&traceStats.hitRays, 0u);
    atomicStore(&traceStats.missRays, 0u);
    atomicStore(&traceStats.invalidDirectionRays, 0u);
    atomicStore(&traceStats.tlasNodeVisits, 0u);
    atomicStore(&traceStats.blasNodeVisits, 0u);
    atomicStore(&traceStats.worldBlasNodeVisits, 0u);
    atomicStore(&traceStats.triangleTests, 0u);
    atomicStore(&traceStats.workgroupQueuePushes, 0u);
    atomicStore(&traceStats.workgroupQueuePops, 0u);
    atomicStore(&traceStats.localStackSpills, 0u);
    atomicStore(&traceStats.overlayNodeTests, 0u);
    atomicStore(&traceStats.overlayEdgeHits, 0u);
    atomicStore(&traceStats.lastHitPrimitive, INVALID_INDEX);
    atomicStore(&traceStats.lastHitInstance, INVALID_INDEX);
    atomicStore(&traceStats.lastHitValid, 0u);
    atomicStore(&traceStats.traversalEarlyOutSkips, 0u);
    atomicStore(&traceStats.debugPixelsReachedValidBLAS, 0u);
    atomicStore(&traceStats.debugPixelsReachedInvalidBLAS, 0u);
    atomicStore(&traceStats.debugPixelsVisitedBLASNode, 0u);
    atomicStore(&traceStats.debugPixelsReadTriangle, 0u);
    atomicStore(&traceStats.debugPixelsRejectedTriangle, 0u);
    atomicStore(&traceStats.debugPixelsAcceptedTriangle, 0u);
    atomicStore(&traceStats.debugPixelsRejectedWorldT, 0u);
    atomicStore(&traceStats.debugPixelsAcceptedCandidate, 0u);
    atomicStore(&traceStats.debugPixelsBLASLeafGroup, 0u);
    atomicStore(&traceStats.debugPixelsInvalidLeafRemap, 0u);
    atomicStore(&traceStats.debugPixelsInvalidLocalRay, 0u);
    atomicStore(&traceStats.dynamicRayFetchCursor, 0u);
}

fn unpack_byte(word: u32, byte_index: u32) -> u32 {
    return (word >> (byte_index * 8u)) & 0xffu;
}

fn extract_byte(word: u32, byte_index: u32) -> u32 {
    return (word >> (byte_index * 8u)) & 0xffu;
}

fn most_significant_bit_index(mask: u32) -> u32 {
    return 31u - countLeadingZeros(mask);
}

fn decode_signed_u8(value: u32) -> i32 {
    return select(i32(value), i32(value) - 256, value >= 128u);
}

fn safe_reciprocal_direction_component(value: f32) -> f32 {
    let epsilon = exp2(-80.0);
    let safeValue = select(select(-epsilon, epsilon, value >= 0.0), value, abs(value) > epsilon);
    return 1.0 / safeValue;
}

fn safe_inverse_direction(direction: vec3<f32>) -> vec3<f32> {
    return vec3<f32>(
        safe_reciprocal_direction_component(direction.x),
        safe_reciprocal_direction_component(direction.y),
        safe_reciprocal_direction_component(direction.z));
}

fn traversal_early_out_enabled() -> bool {
    return uniforms.traceControl.x > 0.5;
}

fn should_skip_group_fast(groupMinT: f32, bestT: f32, earlyOut: bool, earlyOutBias: f32) -> bool {
    return earlyOut && bestT < uniforms.tMax && groupMinT > bestT + earlyOutBias;
}

fn sign_extend_s8x4(x: u32) -> u32 {
    return ((x >> 7u) & 0x01010101u) * 0xffu;
}

#ifdef SWRT_WORLD_CWBVH_SUBGROUP
const SWRT_TRIANGLE_POSTPONING_THRESHOLD_DIVISOR = 5u;

@diagnostic(off, subgroup_uniformity)
fn swrt_subgroup_ballot(pred: bool) -> vec4<u32> {
    return subgroupBallot(pred);
}

@diagnostic(off, subgroup_uniformity)
fn swrt_subgroup_broadcast_first_u32(value: u32) -> u32 {
    return subgroupBroadcastFirst(value);
}

fn swrt_subgroup_ballot_count(mask: vec4<u32>) -> u32 {
    return countOneBits(mask.x) +
        countOneBits(mask.y) +
        countOneBits(mask.z) +
        countOneBits(mask.w);
}

fn swrt_subgroup_active_lane_count(pred: bool) -> u32 {
    return swrt_subgroup_ballot_count(swrt_subgroup_ballot(pred));
}
#endif

fn push_tlas_stack_entry(
    traversalStack: ptr<function, array<vec4<u32>, TLAS_TRAVERSAL_STACK_SIZE>>,
    stackPtr: ptr<function, u32>,
    baseIndex: u32,
    mask: u32,
    minT: f32) -> bool {
    if (*stackPtr) >= TLAS_TRAVERSAL_STACK_SIZE {
        trace_stat_add_local_stack_spill(1u);
        return true;
    }
    (*traversalStack)[(*stackPtr)] = vec4<u32>(baseIndex, mask, bitcast<u32>(minT), 0u);
    (*stackPtr) += 1u;
    return true;
}

fn update_best_hit_local(state: ptr<function, HitState>, candidateT: f32, bary: vec2<f32>, primitiveIndex: u32, instanceIndex: u32) {
    if candidateT >= (*state).bestT {
        return;
    }

    (*state).bestT = candidateT;
    (*state).bestPrimitive = primitiveIndex;
    (*state).bestInstance = instanceIndex;
    (*state).bestBary = bary;
}

fn write_hit_output(rayId: u32, instanceId: u32, triangleId: u32, hitT: f32, bary: vec2<f32>, tlasInstanceIndex: u32, flags: u32) {
    if rayId >= uniforms.rayCount {
        return;
    }
    hitOutput[rayId] = TraceHit(instanceId, triangleId, hitT, bary.x, bary.y, tlasInstanceIndex, flags, 0u);
}

fn debug_mode_rbvh_card_aabb() -> bool {
    return uniforms.debugMode == DEBUG_MODE_RBVH_CARD_AABB;
}

fn debug_mode_heatmap() -> bool {
    return uniforms.debugMode == DEBUG_MODE_HEATMAP;
}

fn mark_debug_rbvh_card_aabb(state: ptr<function, HitState>) {
    if debug_mode_rbvh_card_aabb() && (*state).bestInstance == INVALID_INDEX && (*state).bestPrimitive == INVALID_INDEX {
        (*state).bestPrimitive = DEBUG_HIT_RBVH_CARD_AABB;
    }
}

fn store_trace_result(rayId: u32, hitState: HitState) {
    let hitInstance = hitState.bestInstance;
    if hitInstance != INVALID_INDEX {
        trace_stat_add_hit(hitState.bestPrimitive, hitInstance);
    } else {
        trace_stat_add_miss();
    }

    if debug_mode_heatmap() {
        write_hit_output(rayId, DEBUG_HIT_INSTANCE_HEATMAP, min(hitState.iterationCount, uniforms.heatmapMaxIterations), 0.0, vec2<f32>(0.0), INVALID_INDEX, 0u);
        return;
    }

    if hitInstance != INVALID_INDEX {
#ifdef SWRT_WORLD_CWBVH
        let range = worldInstanceRanges[hitInstance];
        write_hit_output(rayId, range.sceneInstanceId, hitState.bestPrimitive, hitState.bestT, hitState.bestBary, range.instanceIndex, 1u);
#else
        let sceneInstanceId = tlasInstances[hitInstance].sceneInstanceId;
        write_hit_output(rayId, sceneInstanceId, hitState.bestPrimitive, hitState.bestT, hitState.bestBary, hitInstance, 1u);
#endif
    } else if debug_mode_rbvh_card_aabb() && hitState.bestPrimitive == DEBUG_HIT_RBVH_CARD_AABB {
        write_hit_output(rayId, DEBUG_HIT_INSTANCE_MARKER, DEBUG_HIT_RBVH_CARD_AABB, 0.0, vec2<f32>(0.0), INVALID_INDEX, 0u);
    } else {
        write_hit_output(rayId, INVALID_INDEX, INVALID_INDEX, 0.0, vec2<f32>(0.0), INVALID_INDEX, 0u);
    }
}

fn intersect_aabb_interval_invdir(rayOrigin: vec3<f32>, invdir: vec3<f32>, minPoint: vec3<f32>, maxPoint: vec3<f32>) -> vec2<f32> {
    let t0 = (minPoint - rayOrigin) * invdir;
    let t1 = (maxPoint - rayOrigin) * invdir;
    let tsmaller = min(t0, t1);
    let tbigger = max(t0, t1);
    let enterT = max(max(tsmaller.x, tsmaller.y), tsmaller.z);
    let exitT = min(min(tbigger.x, tbigger.y), tbigger.z);
    if enterT > exitT {
        return vec2<f32>(1.0, -1.0);
    }
    return vec2<f32>(enterT, exitT);
}

fn trace_instance_blas_local(rayOrigin: vec3<f32>, rayDirection: vec3<f32>, instanceIndex: u32, entryT: f32, startGroup: vec2<u32>, state: ptr<function, HitState>) {
    if entryT > (*state).bestT {
        return;
    }

    let instance = tlasInstances[instanceIndex];
    let blasHandle = instance.blasHandle;
    if blasHandle == INVALID_INDEX {
        return;
    }

    let metadata = load_blas_metadata_common(blasHandle);
    if metadata.handleId == INVALID_INDEX || metadata.cwbvhNodeCount == 0u {
        return;
    }
    let primitiveMetadata = load_trace_primitive_metadata(blasHandle);

    let inverseTransform = instance.inverseTransform;
    let localOrigin = (inverseTransform * vec4<f32>(rayOrigin, 1.0)).xyz;
    let localDirection = (inverseTransform * vec4<f32>(rayDirection, 0.0)).xyz;
    if any(localOrigin != localOrigin) || any(localDirection != localDirection) || dot(localDirection, localDirection) <= 1.0e-12 {
        return;
    }
    let invdir = safe_inverse_direction(localDirection);
    var octinv = ((select(0u, 4u, localDirection.x < 0.0)) | (select(0u, 2u, localDirection.y < 0.0)) | (select(0u, 1u, localDirection.z < 0.0)));
    octinv = 7u - octinv;

    var traversalStack: array<vec4<u32>, TLAS_TRAVERSAL_STACK_SIZE>;
    var stackPtr: u32 = 0u;
    var ngroup = startGroup;
    var ngroupMinT = 0.0;
    var tgroup = vec2<u32>(0u, 0u);
    var tgroupMinT = uniforms.tMax;
    let orig = localOrigin;
    var tmin = uniforms.tMin;
    var tmax = min((*state).bestT, uniforms.tMax);
    let earlyOut = traversal_early_out_enabled();
    let earlyOutBias = max(uniforms.traceControl.y, 0.0);

    loop {
        if ngroup.y > 0x00FFFFFFu && should_skip_group_fast(ngroupMinT, (*state).bestT, earlyOut, earlyOutBias) {
            ngroup = vec2<u32>(0u, 0u);
        }

        if ngroup.y > 0x00FFFFFFu {
            (*state).iterationCount += 1u;
            let imask = ngroup.y;
            let childBitIndex = most_significant_bit_index(ngroup.y);
            let childNodeBaseIndex = ngroup.x;

            ngroup.y &= ~(1u << childBitIndex);
            if ngroup.y > 0x00FFFFFFu {
                if !push_tlas_stack_entry(&traversalStack, &stackPtr, ngroup.x, ngroup.y, ngroupMinT) {
                    return;
                }
            }

            let slotIndex: u32 = (childBitIndex - 24u) ^ octinv;
            let childNodeIndex: u32 = childNodeBaseIndex + countOneBits(imask & ~(0xFFFFFFFFu << slotIndex));
            let nodeBase = (metadata.cwbvhNodeOffset + childNodeIndex) * 5u;
            let n0 = blasNodes[nodeBase + 0u];
            let n1 = blasNodes[nodeBase + 1u];
            let n2 = blasNodes[nodeBase + 2u];
            let n3 = blasNodes[nodeBase + 3u];
            let n4 = blasNodes[nodeBase + 4u];
            let p = vec3<f32>(bitcast<f32>(n0.x), bitcast<f32>(n0.y), bitcast<f32>(n0.z));
            let e_imask = unpack4xU8(n0.w);
            let octinv4 = octinv * 0x01010101u;
            let adjusted_idirx: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.x) + 127) << 23u) * invdir.x;
            let adjusted_idiry: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.y) + 127) << 23u) * invdir.y;
            let adjusted_idirz: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.z) + 127) << 23u) * invdir.z;
            let origx: f32 = -(orig.x - p.x) * invdir.x;
            let origy: f32 = -(orig.y - p.y) * invdir.y;
            let origz: f32 = -(orig.z - p.z) * invdir.z;
            var hitmask = 0u;
            var childHitBits0 = vec4<u32>(0u);
            var childHitBits1 = vec4<u32>(0u);
            var childMinT0 = vec4<f32>(uniforms.tMax);
            var childMinT1 = vec4<f32>(uniforms.tMax);

            {
                let meta4: u32 = n1.z;
                let is_inner4: u32 = (meta4 & (meta4 << 1u)) & 0x10101010u;
                let inner_mask4: u32 = sign_extend_s8x4(is_inner4 << 3u);
                let bit_index4: u32 = (meta4 ^ (octinv4 & inner_mask4)) & 0x1F1F1F1Fu;
                let child_bits4: u32 = (meta4 >> 5u) & 0x07070707u;
                if child_bits4 != 0u {
                    let swizzledLox = select(n2.x, n3.z, invdir.x < 0.0);
                    let swizzledHix = select(n3.z, n2.x, invdir.x < 0.0);
                    let swizzledLoy = select(n2.z, n4.x, invdir.y < 0.0);
                    let swizzledHiy = select(n4.x, n2.z, invdir.y < 0.0);
                    let swizzledLoz = select(n3.x, n4.z, invdir.z < 0.0);
                    let swizzledHiz = select(n4.z, n3.x, invdir.z < 0.0);
                    for (var i = 0u; i < 4u; i += 1u) {
                        let childBits = extract_byte(child_bits4, i);
                        if childBits != 0u {
                            let shift = i * 8u;
                            let cmin = max(max(max(
                                f32((swizzledLox >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledLoy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledLoz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmin);
                            let cmax = min(min(min(
                                f32((swizzledHix >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledHiy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledHiz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmax);
                            if cmin <= cmax {
                                let bitIndex = extract_byte(bit_index4, i);
                                let hitBits = childBits << bitIndex;
                                childHitBits0[i] = hitBits;
                                childMinT0[i] = cmin;
                                hitmask |= hitBits;
                            }
                        }
                    }
                }
            }
            {
                let meta4: u32 = n1.w;
                let is_inner4: u32 = (meta4 & (meta4 << 1u)) & 0x10101010u;
                let inner_mask4: u32 = sign_extend_s8x4(is_inner4 << 3u);
                let bit_index4: u32 = (meta4 ^ (octinv4 & inner_mask4)) & 0x1F1F1F1Fu;
                let child_bits4: u32 = (meta4 >> 5u) & 0x07070707u;
                if child_bits4 != 0u {
                    let swizzledLox = select(n2.y, n3.w, invdir.x < 0.0);
                    let swizzledHix = select(n3.w, n2.y, invdir.x < 0.0);
                    let swizzledLoy = select(n2.w, n4.y, invdir.y < 0.0);
                    let swizzledHiy = select(n4.y, n2.w, invdir.y < 0.0);
                    let swizzledLoz = select(n3.y, n4.w, invdir.z < 0.0);
                    let swizzledHiz = select(n4.w, n3.y, invdir.z < 0.0);
                    for (var i = 0u; i < 4u; i += 1u) {
                        let childBits = extract_byte(child_bits4, i);
                        if childBits != 0u {
                            let shift = i * 8u;
                            let cmin = max(max(max(
                                f32((swizzledLox >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledLoy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledLoz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmin);
                            let cmax = min(min(min(
                                f32((swizzledHix >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledHiy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledHiz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmax);
                            if cmin <= cmax {
                                let bitIndex = extract_byte(bit_index4, i);
                                let hitBits = childBits << bitIndex;
                                childHitBits1[i] = hitBits;
                                childMinT1[i] = cmin;
                                hitmask |= hitBits;
                            }
                        }
                    }
                }
            }

            var bestBaseIndex = 0u;
            var bestMask = 0u;
            var bestMinT = uniforms.tMax;
            var bestIsInternal = false;
            let internalGroupMask = (hitmask & 0xFF000000u) | e_imask.w;
            let internalPrefixMask = internalGroupMask & 0x00FFFFFFu;
            for (var childSlot = 0u; childSlot < 8u; childSlot += 1u) {
                var hitBits = 0u;
                var childMinT = uniforms.tMax;
                if childSlot < 4u {
                    hitBits = childHitBits0[childSlot];
                    childMinT = childMinT0[childSlot];
                } else {
                    hitBits = childHitBits1[childSlot - 4u];
                    childMinT = childMinT1[childSlot - 4u];
                }
                if hitBits == 0u {
                    continue;
                }
                let childIsInternal = (hitBits & 0xFF000000u) != 0u;
                let baseIndex = select(n1.y, n1.x, childIsInternal);
                let scheduledMask = select(hitBits, hitBits | internalPrefixMask, childIsInternal);
                if bestMask == 0u || childMinT < bestMinT {
                    if bestMask != 0u && !push_tlas_stack_entry(&traversalStack, &stackPtr, bestBaseIndex, bestMask, bestMinT) {
                        return;
                    }
                    bestBaseIndex = baseIndex;
                    bestMask = scheduledMask;
                    bestMinT = childMinT;
                    bestIsInternal = childIsInternal;
                } else if !push_tlas_stack_entry(&traversalStack, &stackPtr, baseIndex, scheduledMask, childMinT) {
                    return;
                }
            }
            if bestMask == 0u {
                ngroup = vec2<u32>(0u, 0u);
                ngroupMinT = uniforms.tMax;
                tgroup = vec2<u32>(0u, 0u);
                tgroupMinT = uniforms.tMax;
            } else if bestIsInternal {
                ngroup = vec2<u32>(bestBaseIndex, bestMask);
                ngroupMinT = bestMinT;
                tgroup = vec2<u32>(0u, 0u);
                tgroupMinT = uniforms.tMax;
            } else {
                ngroup = vec2<u32>(0u, 0u);
                ngroupMinT = uniforms.tMax;
                tgroup = vec2<u32>(bestBaseIndex, bestMask);
                tgroupMinT = bestMinT;
            }
        } else {
            tgroup = ngroup;
            tgroupMinT = ngroupMinT;
            ngroup = vec2<u32>(0u, 0u);
        }

        if tgroup.y != 0u && should_skip_group_fast(tgroupMinT, (*state).bestT, earlyOut, earlyOutBias) {
            tgroup.y = 0u;
        } else {
            while tgroup.y != 0u {
                let primitiveBit = most_significant_bit_index(tgroup.y);
                let localPrimitiveIndex = load_local_leaf_remap(metadata.leafRemapOffset + tgroup.x + primitiveBit);
                trace_blas_primitive_leaf_local_cached(rayOrigin, rayDirection, instance, instanceIndex, metadata, primitiveMetadata, localPrimitiveIndex, localOrigin, localDirection, invdir, state);
                tgroup.y &= ~(1u << primitiveBit);
            }
            tmax = min((*state).bestT, uniforms.tMax);
        }

        if ngroup.y <= 0x00FFFFFFu {
            var foundStackEntry = false;
            loop {
                if stackPtr == 0u {
                    break;
                }
                stackPtr -= 1u;
                let entry = traversalStack[stackPtr];
                let entryMinT = bitcast<f32>(entry.z);
                if should_skip_group_fast(entryMinT, (*state).bestT, earlyOut, earlyOutBias) {
                    continue;
                }
                ngroup = entry.xy;
                ngroupMinT = entryMinT;
                foundStackEntry = true;
                break;
            }
            if !foundStackEntry {
                return;
            }
        }
    }
}

fn trace_instance_blas_any_hit_local(rayOrigin: vec3<f32>, rayDirection: vec3<f32>, instanceIndex: u32, entryT: f32, maxRayT: f32) -> bool {
    if entryT > maxRayT {
        return false;
    }

    let instance = tlasInstances[instanceIndex];
    let blasHandle = instance.blasHandle;
    if blasHandle == INVALID_INDEX {
        return false;
    }

    let metadata = load_blas_metadata_common(blasHandle);
    if metadata.handleId == INVALID_INDEX || metadata.cwbvhNodeCount == 0u {
        return false;
    }
    let primitiveMetadata = load_trace_primitive_metadata(blasHandle);

    let inverseTransform = instance.inverseTransform;
    let localOrigin = (inverseTransform * vec4<f32>(rayOrigin, 1.0)).xyz;
    let localDirection = (inverseTransform * vec4<f32>(rayDirection, 0.0)).xyz;
    if any(localOrigin != localOrigin) || any(localDirection != localDirection) || dot(localDirection, localDirection) <= 1.0e-12 {
        return false;
    }

    let invdir = safe_inverse_direction(localDirection);
    var octinv = ((select(0u, 4u, localDirection.x < 0.0)) | (select(0u, 2u, localDirection.y < 0.0)) | (select(0u, 1u, localDirection.z < 0.0)));
    octinv = 7u - octinv;

    var traversalStack: array<vec4<u32>, TLAS_TRAVERSAL_STACK_SIZE>;
    var stackPtr: u32 = 0u;
    var ngroup = vec2<u32>(0u, 0x80000000u);
    var ngroupMinT = 0.0;
    var tgroup = vec2<u32>(0u, 0u);
    var tgroupMinT = maxRayT;
    let orig = localOrigin;
    let tmin = uniforms.tMin;
    let tmax = maxRayT;

    loop {
        if ngroup.y > 0x00FFFFFFu && ngroupMinT > maxRayT {
            ngroup = vec2<u32>(0u, 0u);
        }

        if ngroup.y > 0x00FFFFFFu {
            let imask = ngroup.y;
            let childBitIndex = most_significant_bit_index(ngroup.y);
            let childNodeBaseIndex = ngroup.x;

            ngroup.y &= ~(1u << childBitIndex);
            if ngroup.y > 0x00FFFFFFu {
                _ = push_tlas_stack_entry(&traversalStack, &stackPtr, ngroup.x, ngroup.y, ngroupMinT);
            }

            let slotIndex: u32 = (childBitIndex - 24u) ^ octinv;
            let childNodeIndex: u32 = childNodeBaseIndex + countOneBits(imask & ~(0xFFFFFFFFu << slotIndex));
            let nodeBase = (metadata.cwbvhNodeOffset + childNodeIndex) * 5u;
            let n0 = blasNodes[nodeBase + 0u];
            let n1 = blasNodes[nodeBase + 1u];
            let n2 = blasNodes[nodeBase + 2u];
            let n3 = blasNodes[nodeBase + 3u];
            let n4 = blasNodes[nodeBase + 4u];
            let p = vec3<f32>(bitcast<f32>(n0.x), bitcast<f32>(n0.y), bitcast<f32>(n0.z));
            let e_imask = unpack4xU8(n0.w);
            let octinv4 = octinv * 0x01010101u;
            let adjusted_idirx: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.x) + 127) << 23u) * invdir.x;
            let adjusted_idiry: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.y) + 127) << 23u) * invdir.y;
            let adjusted_idirz: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.z) + 127) << 23u) * invdir.z;
            let origx: f32 = -(orig.x - p.x) * invdir.x;
            let origy: f32 = -(orig.y - p.y) * invdir.y;
            let origz: f32 = -(orig.z - p.z) * invdir.z;
            var hitmask = 0u;
            var childHitBits0 = vec4<u32>(0u);
            var childHitBits1 = vec4<u32>(0u);
            var childMinT0 = vec4<f32>(maxRayT);
            var childMinT1 = vec4<f32>(maxRayT);

            {
                let meta4: u32 = n1.z;
                let is_inner4: u32 = (meta4 & (meta4 << 1u)) & 0x10101010u;
                let inner_mask4: u32 = sign_extend_s8x4(is_inner4 << 3u);
                let bit_index4: u32 = (meta4 ^ (octinv4 & inner_mask4)) & 0x1F1F1F1Fu;
                let child_bits4: u32 = (meta4 >> 5u) & 0x07070707u;
                if child_bits4 != 0u {
                    let swizzledLox = select(n2.x, n3.z, invdir.x < 0.0);
                    let swizzledHix = select(n3.z, n2.x, invdir.x < 0.0);
                    let swizzledLoy = select(n2.z, n4.x, invdir.y < 0.0);
                    let swizzledHiy = select(n4.x, n2.z, invdir.y < 0.0);
                    let swizzledLoz = select(n3.x, n4.z, invdir.z < 0.0);
                    let swizzledHiz = select(n4.z, n3.x, invdir.z < 0.0);
                    for (var i = 0u; i < 4u; i += 1u) {
                        let childBits = extract_byte(child_bits4, i);
                        if childBits != 0u {
                            let shift = i * 8u;
                            let cmin = max(max(max(
                                f32((swizzledLox >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledLoy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledLoz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmin);
                            let cmax = min(min(min(
                                f32((swizzledHix >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledHiy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledHiz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmax);
                            if cmin <= cmax {
                                let bitIndex = extract_byte(bit_index4, i);
                                let hitBits = childBits << bitIndex;
                                childHitBits0[i] = hitBits;
                                childMinT0[i] = cmin;
                                hitmask |= hitBits;
                            }
                        }
                    }
                }
            }
            {
                let meta4: u32 = n1.w;
                let is_inner4: u32 = (meta4 & (meta4 << 1u)) & 0x10101010u;
                let inner_mask4: u32 = sign_extend_s8x4(is_inner4 << 3u);
                let bit_index4: u32 = (meta4 ^ (octinv4 & inner_mask4)) & 0x1F1F1F1Fu;
                let child_bits4: u32 = (meta4 >> 5u) & 0x07070707u;
                if child_bits4 != 0u {
                    let swizzledLox = select(n2.y, n3.w, invdir.x < 0.0);
                    let swizzledHix = select(n3.w, n2.y, invdir.x < 0.0);
                    let swizzledLoy = select(n2.w, n4.y, invdir.y < 0.0);
                    let swizzledHiy = select(n4.y, n2.w, invdir.y < 0.0);
                    let swizzledLoz = select(n3.y, n4.w, invdir.z < 0.0);
                    let swizzledHiz = select(n4.w, n3.y, invdir.z < 0.0);
                    for (var i = 0u; i < 4u; i += 1u) {
                        let childBits = extract_byte(child_bits4, i);
                        if childBits != 0u {
                            let shift = i * 8u;
                            let cmin = max(max(max(
                                f32((swizzledLox >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledLoy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledLoz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmin);
                            let cmax = min(min(min(
                                f32((swizzledHix >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledHiy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledHiz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmax);
                            if cmin <= cmax {
                                let bitIndex = extract_byte(bit_index4, i);
                                let hitBits = childBits << bitIndex;
                                childHitBits1[i] = hitBits;
                                childMinT1[i] = cmin;
                                hitmask |= hitBits;
                            }
                        }
                    }
                }
            }

            var bestBaseIndex = 0u;
            var bestMask = 0u;
            var bestMinT = maxRayT;
            var bestIsInternal = false;
            let internalGroupMask = (hitmask & 0xFF000000u) | e_imask.w;
            let internalPrefixMask = internalGroupMask & 0x00FFFFFFu;
            for (var childSlot = 0u; childSlot < 8u; childSlot += 1u) {
                var hitBits = 0u;
                var childMinT = maxRayT;
                if childSlot < 4u {
                    hitBits = childHitBits0[childSlot];
                    childMinT = childMinT0[childSlot];
                } else {
                    hitBits = childHitBits1[childSlot - 4u];
                    childMinT = childMinT1[childSlot - 4u];
                }
                if hitBits == 0u {
                    continue;
                }
                let childIsInternal = (hitBits & 0xFF000000u) != 0u;
                let baseIndex = select(n1.y, n1.x, childIsInternal);
                let scheduledMask = select(hitBits, hitBits | internalPrefixMask, childIsInternal);
                if bestMask == 0u || childMinT < bestMinT {
                    if bestMask != 0u {
                        _ = push_tlas_stack_entry(&traversalStack, &stackPtr, bestBaseIndex, bestMask, bestMinT);
                    }
                    bestBaseIndex = baseIndex;
                    bestMask = scheduledMask;
                    bestMinT = childMinT;
                    bestIsInternal = childIsInternal;
                } else {
                    _ = push_tlas_stack_entry(&traversalStack, &stackPtr, baseIndex, scheduledMask, childMinT);
                }
            }
            if bestMask == 0u {
                ngroup = vec2<u32>(0u, 0u);
                ngroupMinT = maxRayT;
                tgroup = vec2<u32>(0u, 0u);
                tgroupMinT = maxRayT;
            } else if bestIsInternal {
                ngroup = vec2<u32>(bestBaseIndex, bestMask);
                ngroupMinT = bestMinT;
                tgroup = vec2<u32>(0u, 0u);
                tgroupMinT = maxRayT;
            } else {
                ngroup = vec2<u32>(0u, 0u);
                ngroupMinT = maxRayT;
                tgroup = vec2<u32>(bestBaseIndex, bestMask);
                tgroupMinT = bestMinT;
            }
        } else {
            tgroup = ngroup;
            tgroupMinT = ngroupMinT;
            ngroup = vec2<u32>(0u, 0u);
        }

        if tgroup.y != 0u && tgroupMinT <= maxRayT {
            while tgroup.y != 0u {
                let primitiveBit = most_significant_bit_index(tgroup.y);
                let localPrimitiveIndex = load_local_leaf_remap(metadata.leafRemapOffset + tgroup.x + primitiveBit);
                if trace_blas_primitive_leaf_any_hit_cached(rayOrigin, rayDirection, instance, metadata, primitiveMetadata, localPrimitiveIndex, localOrigin, localDirection, invdir, maxRayT) {
                    return true;
                }
                tgroup.y &= ~(1u << primitiveBit);
            }
        }

        if ngroup.y <= 0x00FFFFFFu {
            var foundStackEntry = false;
            loop {
                if stackPtr == 0u {
                    break;
                }
                stackPtr -= 1u;
                let entry = traversalStack[stackPtr];
                let entryMinT = bitcast<f32>(entry.z);
                if entryMinT > maxRayT {
                    continue;
                }
                ngroup = entry.xy;
                ngroupMinT = entryMinT;
                foundStackEntry = true;
                break;
            }
            if !foundStackEntry {
                return false;
            }
        }
    }

    return false;
}

#ifdef SWRT_WORLD_CWBVH
fn trace_world_triangle_leaf(
    rayOrigin: vec3<f32>,
    rayDirection: vec3<f32>,
    instanceIndex: u32,
    worldTriangleIndex: u32,
    originalPrimitiveIndex: u32,
    state: ptr<function, HitState>) {
    (*state).iterationCount += 1u;
    if worldTriangleIndex == INVALID_INDEX || originalPrimitiveIndex == INVALID_INDEX {
        return;
    }

    trace_stat_add_triangle_test(1u);
    let transform = blasTriangleTransforms[worldTriangleIndex];
    let hit = intersect_triangle_transform(rayOrigin, rayDirection, transform);
    if hit.z < 0.0 {
        return;
    }

    let worldT = hit.z;
    if worldT < uniforms.tMin || worldT > uniforms.tMax || worldT >= (*state).bestT {
        return;
    }

    update_best_hit_local(state, worldT, hit.xy, originalPrimitiveIndex, instanceIndex);
}

fn push_world_cwbvh_stack_entry(
    traversalStack: ptr<function, array<vec4<u32>, TLAS_TRAVERSAL_STACK_SIZE>>,
    stackPtr: ptr<function, u32>,
    baseIndex: u32,
    mask: u32,
    minT: f32,
    ownerInstanceIndex: u32) -> bool {
    if (*stackPtr) >= TLAS_TRAVERSAL_STACK_SIZE {
        trace_stat_add_local_stack_spill(1u);
        return false;
    }
    (*traversalStack)[(*stackPtr)] = vec4<u32>(baseIndex, mask, bitcast<u32>(minT), ownerInstanceIndex);
    (*stackPtr) += 1u;
    return true;
}

fn trace_stat_add_world_cwbvh_node_visit(ownerInstanceIndex: u32) {
#ifdef SWRT_ENABLE_TRACE_STATS
    if ownerInstanceIndex == INVALID_INDEX {
        atomicAdd(&traceStats.tlasNodeVisits, 1u);
    } else {
        atomicAdd(&traceStats.blasNodeVisits, 1u);
        atomicAdd(&traceStats.worldBlasNodeVisits, 1u);
    }
#endif
}

fn trace_world_cwbvh(rayOrigin: vec3<f32>, rayDirection: vec3<f32>, state: ptr<function, HitState>) {
    if uniforms.tlasNodeCount == 0u {
        return;
    }

    let invdir = safe_inverse_direction(rayDirection);
    var octinv = 7u - ((select(0u, 4u, rayDirection.x < 0.0)) | (select(0u, 2u, rayDirection.y < 0.0)) | (select(0u, 1u, rayDirection.z < 0.0)));
    var traversalStack: array<vec4<u32>, TLAS_TRAVERSAL_STACK_SIZE>;
    var stackPtr = 0u;
    var ngroup = vec2<u32>(0u, 0x80000000u);
    var ngroupMinT = 0.0;
    var ngroupOwnerInstance = INVALID_INDEX;
    var tgroup = vec2<u32>(0u, 0u);
    var tgroupMinT = uniforms.tMax;
    var tgroupOwnerInstance = INVALID_INDEX;
#ifdef SWRT_WORLD_CWBVH_SUBGROUP
    var triangleGroupPostponed = false;
#endif
    let orig = rayOrigin;
    let tmin = uniforms.tMin;
    var tmax = min((*state).bestT, uniforms.tMax);
    let earlyOut = traversal_early_out_enabled();
    let earlyOutBias = max(uniforms.traceControl.y, 0.0);

    loop {
        if ngroup.y > 0x00FFFFFFu && should_skip_group_fast(ngroupMinT, (*state).bestT, earlyOut, earlyOutBias) {
            ngroup = vec2<u32>(0u, 0u);
            ngroupOwnerInstance = INVALID_INDEX;
        }

        if ngroup.y > 0x00FFFFFFu {
            (*state).iterationCount += 1u;
            trace_stat_add_world_cwbvh_node_visit(ngroupOwnerInstance);
            let imask = ngroup.y;
            let childBitIndex = most_significant_bit_index(ngroup.y);
            let childNodeBaseIndex = ngroup.x;

            ngroup.y &= ~(1u << childBitIndex);
            if ngroup.y > 0x00FFFFFFu {
                if !push_world_cwbvh_stack_entry(&traversalStack, &stackPtr, ngroup.x, ngroup.y, ngroupMinT, ngroupOwnerInstance) {
                    return;
                }
            }

            let slotIndex: u32 = (childBitIndex - 24u) ^ octinv;
            let childNodeIndex: u32 = childNodeBaseIndex + countOneBits(imask & ~(0xFFFFFFFFu << slotIndex));
            let nodeBase = childNodeIndex * 5u;
            let n0 = tlasNodes[nodeBase + 0u];
            let n1 = tlasNodes[nodeBase + 1u];
            let n2 = tlasNodes[nodeBase + 2u];
            let n3 = tlasNodes[nodeBase + 3u];
            let n4 = tlasNodes[nodeBase + 4u];
            let p = vec3<f32>(bitcast<f32>(n0.x), bitcast<f32>(n0.y), bitcast<f32>(n0.z));
            let e_imask = unpack4xU8(n0.w);
            let octinv4 = octinv * 0x01010101u;
            let adjusted_idirx: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.x) + 127) << 23u) * invdir.x;
            let adjusted_idiry: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.y) + 127) << 23u) * invdir.y;
            let adjusted_idirz: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.z) + 127) << 23u) * invdir.z;
            let origx: f32 = -(orig.x - p.x) * invdir.x;
            let origy: f32 = -(orig.y - p.y) * invdir.y;
            let origz: f32 = -(orig.z - p.z) * invdir.z;
            var hitmask = 0u;
            var childHitBits0 = vec4<u32>(0u);
            var childHitBits1 = vec4<u32>(0u);
            var childMinT0 = vec4<f32>(uniforms.tMax);
            var childMinT1 = vec4<f32>(uniforms.tMax);

            {
                let meta4: u32 = n1.z;
                let is_inner4: u32 = (meta4 & (meta4 << 1u)) & 0x10101010u;
                let inner_mask4: u32 = sign_extend_s8x4(is_inner4 << 3u);
                let bit_index4: u32 = (meta4 ^ (octinv4 & inner_mask4)) & 0x1F1F1F1Fu;
                let child_bits4: u32 = (meta4 >> 5u) & 0x07070707u;
                if child_bits4 != 0u {
                    let swizzledLox = select(n2.x, n3.z, invdir.x < 0.0);
                    let swizzledHix = select(n3.z, n2.x, invdir.x < 0.0);
                    let swizzledLoy = select(n2.z, n4.x, invdir.y < 0.0);
                    let swizzledHiy = select(n4.x, n2.z, invdir.y < 0.0);
                    let swizzledLoz = select(n3.x, n4.z, invdir.z < 0.0);
                    let swizzledHiz = select(n4.z, n3.x, invdir.z < 0.0);
                    for (var i = 0u; i < 4u; i += 1u) {
                        let childBits = extract_byte(child_bits4, i);
                        if childBits != 0u {
                            let shift = i * 8u;
                            let cmin = max(max(max(
                                f32((swizzledLox >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledLoy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledLoz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmin);
                            let cmax = min(min(min(
                                f32((swizzledHix >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledHiy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledHiz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmax);
                            if cmin <= cmax {
                                let bitIndex = extract_byte(bit_index4, i);
                                let hitBits = childBits << bitIndex;
                                childHitBits0[i] = hitBits;
                                childMinT0[i] = cmin;
                                hitmask |= hitBits;
                            }
                        }
                    }
                }
            }
            {
                let meta4: u32 = n1.w;
                let is_inner4: u32 = (meta4 & (meta4 << 1u)) & 0x10101010u;
                let inner_mask4: u32 = sign_extend_s8x4(is_inner4 << 3u);
                let bit_index4: u32 = (meta4 ^ (octinv4 & inner_mask4)) & 0x1F1F1F1Fu;
                let child_bits4: u32 = (meta4 >> 5u) & 0x07070707u;
                if child_bits4 != 0u {
                    let swizzledLox = select(n2.y, n3.w, invdir.x < 0.0);
                    let swizzledHix = select(n3.w, n2.y, invdir.x < 0.0);
                    let swizzledLoy = select(n2.w, n4.y, invdir.y < 0.0);
                    let swizzledHiy = select(n4.y, n2.w, invdir.y < 0.0);
                    let swizzledLoz = select(n3.y, n4.w, invdir.z < 0.0);
                    let swizzledHiz = select(n4.w, n3.y, invdir.z < 0.0);
                    for (var i = 0u; i < 4u; i += 1u) {
                        let childBits = extract_byte(child_bits4, i);
                        if childBits != 0u {
                            let shift = i * 8u;
                            let cmin = max(max(max(
                                f32((swizzledLox >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledLoy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledLoz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmin);
                            let cmax = min(min(min(
                                f32((swizzledHix >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledHiy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledHiz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmax);
                            if cmin <= cmax {
                                let bitIndex = extract_byte(bit_index4, i);
                                let hitBits = childBits << bitIndex;
                                childHitBits1[i] = hitBits;
                                childMinT1[i] = cmin;
                                hitmask |= hitBits;
                            }
                        }
                    }
                }
            }

            var bestBaseIndex = 0u;
            var bestMask = 0u;
            var bestMinT = uniforms.tMax;
            var bestIsInternal = false;
            let internalGroupMask = (hitmask & 0xFF000000u) | e_imask.w;
            let internalPrefixMask = internalGroupMask & 0x00FFFFFFu;
            for (var childSlot = 0u; childSlot < 8u; childSlot += 1u) {
                var hitBits = 0u;
                var childMinT = uniforms.tMax;
                if childSlot < 4u {
                    hitBits = childHitBits0[childSlot];
                    childMinT = childMinT0[childSlot];
                } else {
                    hitBits = childHitBits1[childSlot - 4u];
                    childMinT = childMinT1[childSlot - 4u];
                }
                if hitBits == 0u {
                    continue;
                }
                let childIsInternal = (hitBits & 0xFF000000u) != 0u;
                let baseIndex = select(n1.y, n1.x, childIsInternal);
                let scheduledMask = select(hitBits, hitBits | internalPrefixMask, childIsInternal);
                if bestMask == 0u || childMinT < bestMinT {
                    if bestMask != 0u && !push_world_cwbvh_stack_entry(&traversalStack, &stackPtr, bestBaseIndex, bestMask, bestMinT, ngroupOwnerInstance) {
                        return;
                    }
                    bestBaseIndex = baseIndex;
                    bestMask = scheduledMask;
                    bestMinT = childMinT;
                    bestIsInternal = childIsInternal;
                } else if !push_world_cwbvh_stack_entry(&traversalStack, &stackPtr, baseIndex, scheduledMask, childMinT, ngroupOwnerInstance) {
                    return;
                }
            }
            if bestMask == 0u {
                ngroup = vec2<u32>(0u, 0u);
                ngroupMinT = uniforms.tMax;
                ngroupOwnerInstance = INVALID_INDEX;
                tgroup = vec2<u32>(0u, 0u);
                tgroupMinT = uniforms.tMax;
                tgroupOwnerInstance = INVALID_INDEX;
            } else if bestIsInternal {
                ngroup = vec2<u32>(bestBaseIndex, bestMask);
                ngroupMinT = bestMinT;
                tgroup = vec2<u32>(0u, 0u);
                tgroupMinT = uniforms.tMax;
                tgroupOwnerInstance = INVALID_INDEX;
            } else {
                let leafOwnerInstance = ngroupOwnerInstance;
                ngroup = vec2<u32>(0u, 0u);
                ngroupMinT = uniforms.tMax;
                ngroupOwnerInstance = INVALID_INDEX;
                tgroup = vec2<u32>(bestBaseIndex, bestMask);
                tgroupMinT = bestMinT;
                tgroupOwnerInstance = leafOwnerInstance;
            }
        } else {
            tgroup = ngroup;
            tgroupMinT = ngroupMinT;
            tgroupOwnerInstance = ngroupOwnerInstance;
            ngroup = vec2<u32>(0u, 0u);
            ngroupOwnerInstance = INVALID_INDEX;
        }

        if tgroup.y != 0u && should_skip_group_fast(tgroupMinT, (*state).bestT, earlyOut, earlyOutBias) {
            tgroup.y = 0u;
            tgroupOwnerInstance = INVALID_INDEX;
        } else {
#ifdef SWRT_WORLD_CWBVH_SUBGROUP
            let subgroupTraversalLaneCount = swrt_subgroup_active_lane_count(true);
#endif
            if tgroupOwnerInstance == INVALID_INDEX {
                var bestPendingGroup = vec2<u32>(0u, 0u);
                var bestPendingMinT = uniforms.tMax;
                var bestPendingOwner = INVALID_INDEX;
                while tgroup.y != 0u {
                    let leafBit = most_significant_bit_index(tgroup.y);
                    let instanceIndex = tlasLeafInstances[tgroup.x + leafBit];
                    if instanceIndex != INVALID_INDEX {
                        let leaf = tlasInstanceAabbs[instanceIndex];
                        let worldInterval = intersect_aabb_interval_invdir(rayOrigin, invdir, leaf.minPoint, leaf.maxPoint);
                        let nearT = max(worldInterval.x, uniforms.tMin);
                        let farT = min(worldInterval.y, min((*state).bestT, uniforms.tMax));
                        if leaf.blasHandle != INVALID_INDEX && nearT <= farT {
                            let range = worldInstanceRanges[leaf.instanceIndex];
                            if range.valid != 0u && range.worldCWBVHNodeCount != 0u && nearT <= (*state).bestT {
                                let candidateGroup = vec2<u32>(range.worldCWBVHNodeOffset, 0x80000000u);
                                if bestPendingGroup.y == 0u || nearT < bestPendingMinT {
                                    if bestPendingGroup.y != 0u && !push_world_cwbvh_stack_entry(&traversalStack, &stackPtr, bestPendingGroup.x, bestPendingGroup.y, bestPendingMinT, bestPendingOwner) {
                                        return;
                                    }
                                    bestPendingGroup = candidateGroup;
                                    bestPendingMinT = nearT;
                                    bestPendingOwner = leaf.instanceIndex;
                                } else if !push_world_cwbvh_stack_entry(&traversalStack, &stackPtr, candidateGroup.x, candidateGroup.y, nearT, leaf.instanceIndex) {
                                    return;
                                }
                            }
                        }
                    }
                    tgroup.y &= ~(1u << leafBit);
                }
                if bestPendingGroup.y != 0u {
                    ngroup = bestPendingGroup;
                    ngroupMinT = bestPendingMinT;
                    ngroupOwnerInstance = bestPendingOwner;
                }
            } else {
#ifdef SWRT_WORLD_CWBVH_SUBGROUP
                let subgroupTriangleLaneCount = swrt_subgroup_active_lane_count(tgroup.y != 0u);
                let subgroupTrianglePostponeThreshold = subgroupTraversalLaneCount / SWRT_TRIANGLE_POSTPONING_THRESHOLD_DIVISOR;
                if subgroupTrianglePostponeThreshold > 0u &&
                    subgroupTriangleLaneCount < subgroupTrianglePostponeThreshold &&
                    tgroup.y != 0u &&
                    !triangleGroupPostponed {
                    if !push_world_cwbvh_stack_entry(&traversalStack, &stackPtr, tgroup.x, tgroup.y, tgroupMinT, tgroupOwnerInstance) {
                        return;
                    }
                    triangleGroupPostponed = true;
                    tgroup.y = 0u;
                } else {
#endif
                    while tgroup.y != 0u {
                        let primitiveBit = most_significant_bit_index(tgroup.y);
                        let leaf = blasLeafRemap[tgroup.x + primitiveBit];
                        trace_world_triangle_leaf(rayOrigin, rayDirection, tgroupOwnerInstance, leaf.x, leaf.y, state);
                        tgroup.y &= ~(1u << primitiveBit);
                    }
#ifdef SWRT_WORLD_CWBVH_SUBGROUP
                    triangleGroupPostponed = false;
                }
#endif
                tmax = min((*state).bestT, uniforms.tMax);
            }
        }

        if ngroup.y <= 0x00FFFFFFu {
            var foundStackEntry = false;
            loop {
                if stackPtr == 0u {
                    break;
                }
                stackPtr -= 1u;
                let entry = traversalStack[stackPtr];
                let entryMinT = bitcast<f32>(entry.z);
                if should_skip_group_fast(entryMinT, (*state).bestT, earlyOut, earlyOutBias) {
                    continue;
                }
                ngroup = entry.xy;
                ngroupMinT = entryMinT;
                ngroupOwnerInstance = entry.w;
                foundStackEntry = true;
                break;
            }
            if !foundStackEntry {
                return;
            }
        }
    }
}
#endif

#ifndef SWRT_WORLD_CWBVH
fn trace_tlas_local(rayOrigin: vec3<f32>, rayDirection: vec3<f32>, state: ptr<function, HitState>) {
    if uniforms.tlasNodeCount == 0u {
        return;
    }

    let invdir = safe_inverse_direction(rayDirection);
    var octinv = 7u - ((select(0u, 4u, rayDirection.x < 0.0)) | (select(0u, 2u, rayDirection.y < 0.0)) | (select(0u, 1u, rayDirection.z < 0.0)));
    var traversalStack: array<vec4<u32>, TLAS_TRAVERSAL_STACK_SIZE>;
    var stackPtr = 0u;
    var ngroup = vec2<u32>(0u, 0x80000000u);
    var ngroupMinT = 0.0;
    var tgroup = vec2<u32>(0u, 0u);
    var tgroupMinT = uniforms.tMax;
    let orig = rayOrigin;
    let tmin = uniforms.tMin;
    var tmax = min((*state).bestT, uniforms.tMax);
    let earlyOut = traversal_early_out_enabled();
    let earlyOutBias = max(uniforms.traceControl.y, 0.0);

    loop {
        if ngroup.y > 0x00FFFFFFu && should_skip_group_fast(ngroupMinT, (*state).bestT, earlyOut, earlyOutBias) {
            ngroup = vec2<u32>(0u, 0u);
        }

        if ngroup.y > 0x00FFFFFFu {
            (*state).iterationCount += 1u;
            let imask = ngroup.y;
            let childBitIndex = most_significant_bit_index(ngroup.y);
            let childNodeBaseIndex = ngroup.x;

            ngroup.y &= ~(1u << childBitIndex);
            if ngroup.y > 0x00FFFFFFu {
                if !push_tlas_stack_entry(&traversalStack, &stackPtr, ngroup.x, ngroup.y, ngroupMinT) {
                    return;
                }
            }

            let slotIndex: u32 = (childBitIndex - 24u) ^ octinv;
            let childNodeIndex: u32 = childNodeBaseIndex + countOneBits(imask & ~(0xFFFFFFFFu << slotIndex));
            let nodeBase = childNodeIndex * 5u;
            let n0 = tlasNodes[nodeBase + 0u];
            let n1 = tlasNodes[nodeBase + 1u];
            let n2 = tlasNodes[nodeBase + 2u];
            let n3 = tlasNodes[nodeBase + 3u];
            let n4 = tlasNodes[nodeBase + 4u];
            let p = vec3<f32>(bitcast<f32>(n0.x), bitcast<f32>(n0.y), bitcast<f32>(n0.z));
            let e_imask = unpack4xU8(n0.w);
            let octinv4 = octinv * 0x01010101u;
            let adjusted_idirx: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.x) + 127) << 23u) * invdir.x;
            let adjusted_idiry: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.y) + 127) << 23u) * invdir.y;
            let adjusted_idirz: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.z) + 127) << 23u) * invdir.z;
            let origx: f32 = -(orig.x - p.x) * invdir.x;
            let origy: f32 = -(orig.y - p.y) * invdir.y;
            let origz: f32 = -(orig.z - p.z) * invdir.z;
            var hitmask = 0u;
            var childHitBits0 = vec4<u32>(0u);
            var childHitBits1 = vec4<u32>(0u);
            var childMinT0 = vec4<f32>(uniforms.tMax);
            var childMinT1 = vec4<f32>(uniforms.tMax);

            {
                let meta4: u32 = n1.z;
                let is_inner4: u32 = (meta4 & (meta4 << 1u)) & 0x10101010u;
                let inner_mask4: u32 = sign_extend_s8x4(is_inner4 << 3u);
                let bit_index4: u32 = (meta4 ^ (octinv4 & inner_mask4)) & 0x1F1F1F1Fu;
                let child_bits4: u32 = (meta4 >> 5u) & 0x07070707u;
                if child_bits4 != 0u {
                    let swizzledLox = select(n2.x, n3.z, invdir.x < 0.0);
                    let swizzledHix = select(n3.z, n2.x, invdir.x < 0.0);
                    let swizzledLoy = select(n2.z, n4.x, invdir.y < 0.0);
                    let swizzledHiy = select(n4.x, n2.z, invdir.y < 0.0);
                    let swizzledLoz = select(n3.x, n4.z, invdir.z < 0.0);
                    let swizzledHiz = select(n4.z, n3.x, invdir.z < 0.0);
                    for (var i = 0u; i < 4u; i += 1u) {
                        let childBits = extract_byte(child_bits4, i);
                        if childBits != 0u {
                            let shift = i * 8u;
                            let cmin = max(max(max(
                                f32((swizzledLox >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledLoy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledLoz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmin);
                            let cmax = min(min(min(
                                f32((swizzledHix >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledHiy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledHiz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmax);
                            if cmin <= cmax {
                                let bitIndex = extract_byte(bit_index4, i);
                                let hitBits = childBits << bitIndex;
                                childHitBits0[i] = hitBits;
                                childMinT0[i] = cmin;
                                hitmask |= hitBits;
                            }
                        }
                    }
                }
            }
            {
                let meta4: u32 = n1.w;
                let is_inner4: u32 = (meta4 & (meta4 << 1u)) & 0x10101010u;
                let inner_mask4: u32 = sign_extend_s8x4(is_inner4 << 3u);
                let bit_index4: u32 = (meta4 ^ (octinv4 & inner_mask4)) & 0x1F1F1F1Fu;
                let child_bits4: u32 = (meta4 >> 5u) & 0x07070707u;
                if child_bits4 != 0u {
                    let swizzledLox = select(n2.y, n3.w, invdir.x < 0.0);
                    let swizzledHix = select(n3.w, n2.y, invdir.x < 0.0);
                    let swizzledLoy = select(n2.w, n4.y, invdir.y < 0.0);
                    let swizzledHiy = select(n4.y, n2.w, invdir.y < 0.0);
                    let swizzledLoz = select(n3.y, n4.w, invdir.z < 0.0);
                    let swizzledHiz = select(n4.w, n3.y, invdir.z < 0.0);
                    for (var i = 0u; i < 4u; i += 1u) {
                        let childBits = extract_byte(child_bits4, i);
                        if childBits != 0u {
                            let shift = i * 8u;
                            let cmin = max(max(max(
                                f32((swizzledLox >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledLoy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledLoz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmin);
                            let cmax = min(min(min(
                                f32((swizzledHix >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledHiy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledHiz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmax);
                            if cmin <= cmax {
                                let bitIndex = extract_byte(bit_index4, i);
                                let hitBits = childBits << bitIndex;
                                childHitBits1[i] = hitBits;
                                childMinT1[i] = cmin;
                                hitmask |= hitBits;
                            }
                        }
                    }
                }
            }

            var bestBaseIndex = 0u;
            var bestMask = 0u;
            var bestMinT = uniforms.tMax;
            var bestIsInternal = false;
            let internalGroupMask = (hitmask & 0xFF000000u) | e_imask.w;
            let internalPrefixMask = internalGroupMask & 0x00FFFFFFu;
            for (var childSlot = 0u; childSlot < 8u; childSlot += 1u) {
                var hitBits = 0u;
                var childMinT = uniforms.tMax;
                if childSlot < 4u {
                    hitBits = childHitBits0[childSlot];
                    childMinT = childMinT0[childSlot];
                } else {
                    hitBits = childHitBits1[childSlot - 4u];
                    childMinT = childMinT1[childSlot - 4u];
                }
                if hitBits == 0u {
                    continue;
                }
                let childIsInternal = (hitBits & 0xFF000000u) != 0u;
                let baseIndex = select(n1.y, n1.x, childIsInternal);
                let scheduledMask = select(hitBits, hitBits | internalPrefixMask, childIsInternal);
                if bestMask == 0u || childMinT < bestMinT {
                    if bestMask != 0u && !push_tlas_stack_entry(&traversalStack, &stackPtr, bestBaseIndex, bestMask, bestMinT) {
                        return;
                    }
                    bestBaseIndex = baseIndex;
                    bestMask = scheduledMask;
                    bestMinT = childMinT;
                    bestIsInternal = childIsInternal;
                } else if !push_tlas_stack_entry(&traversalStack, &stackPtr, baseIndex, scheduledMask, childMinT) {
                    return;
                }
            }
            if bestMask == 0u {
                ngroup = vec2<u32>(0u, 0u);
                ngroupMinT = uniforms.tMax;
                tgroup = vec2<u32>(0u, 0u);
                tgroupMinT = uniforms.tMax;
            } else if bestIsInternal {
                ngroup = vec2<u32>(bestBaseIndex, bestMask);
                ngroupMinT = bestMinT;
                tgroup = vec2<u32>(0u, 0u);
                tgroupMinT = uniforms.tMax;
            } else {
                ngroup = vec2<u32>(0u, 0u);
                ngroupMinT = uniforms.tMax;
                tgroup = vec2<u32>(bestBaseIndex, bestMask);
                tgroupMinT = bestMinT;
            }
        } else {
            tgroup = ngroup;
            tgroupMinT = ngroupMinT;
            ngroup = vec2<u32>(0u, 0u);
        }

        if tgroup.y != 0u && should_skip_group_fast(tgroupMinT, (*state).bestT, earlyOut, earlyOutBias) {
            tgroup.y = 0u;
        } else {
            while tgroup.y != 0u {
                let leafBit = most_significant_bit_index(tgroup.y);
                let instanceIndex = tlasLeafInstances[tgroup.x + leafBit];
                if instanceIndex != INVALID_INDEX {
                    let leaf = tlasInstanceAabbs[instanceIndex];
                    let worldInterval = intersect_aabb_interval_invdir(rayOrigin, invdir, leaf.minPoint, leaf.maxPoint);
                    let nearT = max(worldInterval.x, uniforms.tMin);
                    let farT = min(worldInterval.y, min((*state).bestT, uniforms.tMax));
                    if leaf.blasHandle != INVALID_INDEX && nearT <= farT {
                        trace_instance_blas_local(
                            rayOrigin,
                            rayDirection,
                            leaf.instanceIndex,
                            nearT,
                            vec2<u32>(0u, 0x80000000u),
                            state);
                        tmax = min(tmax, (*state).bestT);
                    }
                }
                tgroup.y &= ~(1u << leafBit);
            }
        }

        if ngroup.y <= 0x00FFFFFFu {
            var foundStackEntry = false;
            loop {
                if stackPtr == 0u {
                    break;
                }
                stackPtr -= 1u;
                let entry = traversalStack[stackPtr];
                let entryMinT = bitcast<f32>(entry.z);
                if should_skip_group_fast(entryMinT, (*state).bestT, earlyOut, earlyOutBias) {
                    continue;
                }
                ngroup = entry.xy;
                ngroupMinT = entryMinT;
                foundStackEntry = true;
                break;
            }
            if !foundStackEntry {
                return;
            }
        }
    }
}

fn trace_tlas_any_hit_local(rayOrigin: vec3<f32>, rayDirection: vec3<f32>, maxRayT: f32) -> bool {
    if uniforms.tlasNodeCount == 0u {
        return false;
    }

    let invdir = safe_inverse_direction(rayDirection);
    var octinv = 7u - ((select(0u, 4u, rayDirection.x < 0.0)) | (select(0u, 2u, rayDirection.y < 0.0)) | (select(0u, 1u, rayDirection.z < 0.0)));
    var traversalStack: array<vec4<u32>, TLAS_TRAVERSAL_STACK_SIZE>;
    var stackPtr: u32 = 0u;
    var ngroup = vec2<u32>(0u, 0x80000000u);
    var ngroupMinT = 0.0;
    var tgroup = vec2<u32>(0u, 0u);
    var tgroupMinT = maxRayT;
    let orig = rayOrigin;
    let tmin = uniforms.tMin;
    let tmax = maxRayT;

    loop {
        if ngroup.y > 0x00FFFFFFu && ngroupMinT > maxRayT {
            ngroup = vec2<u32>(0u, 0u);
        }

        if ngroup.y > 0x00FFFFFFu {
            let imask = ngroup.y;
            let childBitIndex = most_significant_bit_index(ngroup.y);
            let childNodeBaseIndex = ngroup.x;

            ngroup.y &= ~(1u << childBitIndex);
            if ngroup.y > 0x00FFFFFFu {
                if !push_tlas_stack_entry(&traversalStack, &stackPtr, ngroup.x, ngroup.y, ngroupMinT) {
                    return false;
                }
            }

            let slotIndex: u32 = (childBitIndex - 24u) ^ octinv;
            let childNodeIndex: u32 = childNodeBaseIndex + countOneBits(imask & ~(0xFFFFFFFFu << slotIndex));
            let nodeBase = childNodeIndex * 5u;
            let n0 = tlasNodes[nodeBase + 0u];
            let n1 = tlasNodes[nodeBase + 1u];
            let n2 = tlasNodes[nodeBase + 2u];
            let n3 = tlasNodes[nodeBase + 3u];
            let n4 = tlasNodes[nodeBase + 4u];
            let p = vec3<f32>(bitcast<f32>(n0.x), bitcast<f32>(n0.y), bitcast<f32>(n0.z));
            let e_imask = unpack4xU8(n0.w);
            let octinv4 = octinv * 0x01010101u;
            let adjusted_idirx: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.x) + 127) << 23u) * invdir.x;
            let adjusted_idiry: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.y) + 127) << 23u) * invdir.y;
            let adjusted_idirz: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.z) + 127) << 23u) * invdir.z;
            let origx: f32 = -(orig.x - p.x) * invdir.x;
            let origy: f32 = -(orig.y - p.y) * invdir.y;
            let origz: f32 = -(orig.z - p.z) * invdir.z;
            var hitmask = 0u;
            var childHitBits0 = vec4<u32>(0u);
            var childHitBits1 = vec4<u32>(0u);
            var childMinT0 = vec4<f32>(maxRayT);
            var childMinT1 = vec4<f32>(maxRayT);

            {
                let meta4: u32 = n1.z;
                let is_inner4: u32 = (meta4 & (meta4 << 1u)) & 0x10101010u;
                let inner_mask4: u32 = sign_extend_s8x4(is_inner4 << 3u);
                let bit_index4: u32 = (meta4 ^ (octinv4 & inner_mask4)) & 0x1F1F1F1Fu;
                let child_bits4: u32 = (meta4 >> 5u) & 0x07070707u;
                if child_bits4 != 0u {
                    let swizzledLox = select(n2.x, n3.z, invdir.x < 0.0);
                    let swizzledHix = select(n3.z, n2.x, invdir.x < 0.0);
                    let swizzledLoy = select(n2.z, n4.x, invdir.y < 0.0);
                    let swizzledHiy = select(n4.x, n2.z, invdir.y < 0.0);
                    let swizzledLoz = select(n3.x, n4.z, invdir.z < 0.0);
                    let swizzledHiz = select(n4.z, n3.x, invdir.z < 0.0);
                    for (var i = 0u; i < 4u; i += 1u) {
                        let childBits = extract_byte(child_bits4, i);
                        if childBits != 0u {
                            let shift = i * 8u;
                            let cmin = max(max(max(
                                f32((swizzledLox >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledLoy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledLoz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmin);
                            let cmax = min(min(min(
                                f32((swizzledHix >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledHiy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledHiz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmax);
                            if cmin <= cmax {
                                let bitIndex = extract_byte(bit_index4, i);
                                let hitBits = childBits << bitIndex;
                                childHitBits0[i] = hitBits;
                                childMinT0[i] = cmin;
                                hitmask |= hitBits;
                            }
                        }
                    }
                }
            }
            {
                let meta4: u32 = n1.w;
                let is_inner4: u32 = (meta4 & (meta4 << 1u)) & 0x10101010u;
                let inner_mask4: u32 = sign_extend_s8x4(is_inner4 << 3u);
                let bit_index4: u32 = (meta4 ^ (octinv4 & inner_mask4)) & 0x1F1F1F1Fu;
                let child_bits4: u32 = (meta4 >> 5u) & 0x07070707u;
                if child_bits4 != 0u {
                    let swizzledLox = select(n2.y, n3.w, invdir.x < 0.0);
                    let swizzledHix = select(n3.w, n2.y, invdir.x < 0.0);
                    let swizzledLoy = select(n2.w, n4.y, invdir.y < 0.0);
                    let swizzledHiy = select(n4.y, n2.w, invdir.y < 0.0);
                    let swizzledLoz = select(n3.y, n4.w, invdir.z < 0.0);
                    let swizzledHiz = select(n4.w, n3.y, invdir.z < 0.0);
                    for (var i = 0u; i < 4u; i += 1u) {
                        let childBits = extract_byte(child_bits4, i);
                        if childBits != 0u {
                            let shift = i * 8u;
                            let cmin = max(max(max(
                                f32((swizzledLox >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledLoy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledLoz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmin);
                            let cmax = min(min(min(
                                f32((swizzledHix >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledHiy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledHiz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmax);
                            if cmin <= cmax {
                                let bitIndex = extract_byte(bit_index4, i);
                                let hitBits = childBits << bitIndex;
                                childHitBits1[i] = hitBits;
                                childMinT1[i] = cmin;
                                hitmask |= hitBits;
                            }
                        }
                    }
                }
            }

            var bestBaseIndex = 0u;
            var bestMask = 0u;
            var bestMinT = maxRayT;
            var bestIsInternal = false;
            let internalGroupMask = (hitmask & 0xFF000000u) | e_imask.w;
            let internalPrefixMask = internalGroupMask & 0x00FFFFFFu;
            for (var childSlot = 0u; childSlot < 8u; childSlot += 1u) {
                var hitBits = 0u;
                var childMinT = maxRayT;
                if childSlot < 4u {
                    hitBits = childHitBits0[childSlot];
                    childMinT = childMinT0[childSlot];
                } else {
                    hitBits = childHitBits1[childSlot - 4u];
                    childMinT = childMinT1[childSlot - 4u];
                }
                if hitBits == 0u {
                    continue;
                }
                let childIsInternal = (hitBits & 0xFF000000u) != 0u;
                let baseIndex = select(n1.y, n1.x, childIsInternal);
                let scheduledMask = select(hitBits, hitBits | internalPrefixMask, childIsInternal);
                if bestMask == 0u || childMinT < bestMinT {
                    if bestMask != 0u && !push_tlas_stack_entry(&traversalStack, &stackPtr, bestBaseIndex, bestMask, bestMinT) {
                        return false;
                    }
                    bestBaseIndex = baseIndex;
                    bestMask = scheduledMask;
                    bestMinT = childMinT;
                    bestIsInternal = childIsInternal;
                } else if !push_tlas_stack_entry(&traversalStack, &stackPtr, baseIndex, scheduledMask, childMinT) {
                    return false;
                }
            }
            if bestMask == 0u {
                ngroup = vec2<u32>(0u, 0u);
                ngroupMinT = maxRayT;
                tgroup = vec2<u32>(0u, 0u);
                tgroupMinT = maxRayT;
            } else if bestIsInternal {
                ngroup = vec2<u32>(bestBaseIndex, bestMask);
                ngroupMinT = bestMinT;
                tgroup = vec2<u32>(0u, 0u);
                tgroupMinT = maxRayT;
            } else {
                ngroup = vec2<u32>(0u, 0u);
                ngroupMinT = maxRayT;
                tgroup = vec2<u32>(bestBaseIndex, bestMask);
                tgroupMinT = bestMinT;
            }
        } else {
            tgroup = ngroup;
            tgroupMinT = ngroupMinT;
            ngroup = vec2<u32>(0u, 0u);
        }

        if tgroup.y != 0u && tgroupMinT <= maxRayT {
            while tgroup.y != 0u {
                let leafBit = most_significant_bit_index(tgroup.y);
                let instanceIndex = tlasLeafInstances[tgroup.x + leafBit];
                if instanceIndex != INVALID_INDEX {
                    let leaf = tlasInstanceAabbs[instanceIndex];
                    let worldInterval = intersect_aabb_interval_invdir(rayOrigin, invdir, leaf.minPoint, leaf.maxPoint);
                    let nearT = max(worldInterval.x, uniforms.tMin);
                    let farT = min(worldInterval.y, maxRayT);
                    if leaf.blasHandle != INVALID_INDEX && nearT <= farT {
                        if trace_instance_blas_any_hit_local(rayOrigin, rayDirection, leaf.instanceIndex, nearT, maxRayT) {
                            return true;
                        }
                    }
                }
                tgroup.y &= ~(1u << leafBit);
            }
        }

        if ngroup.y <= 0x00FFFFFFu {
            var foundStackEntry = false;
            loop {
                if stackPtr == 0u {
                    break;
                }
                stackPtr -= 1u;
                let entry = traversalStack[stackPtr];
                let entryMinT = bitcast<f32>(entry.z);
                if entryMinT > maxRayT {
                    continue;
                }
                ngroup = entry.xy;
                ngroupMinT = entryMinT;
                foundStackEntry = true;
                break;
            }
            if !foundStackEntry {
                return false;
            }
        }
    }

    return false;
}

struct Bvh4StackItem {
    index: u32,
    tBits: u32,
    kind: u32,
    pad: u32,
};

fn select_nearest4(t: vec4<f32>, validMask: u32) -> u32 {
    var bestSlot = INVALID_INDEX;
    var bestT = uniforms.tMax;
    if (((validMask & 1u) != 0u) && (t.x < bestT)) {
        bestT = t.x;
        bestSlot = 0u;
    }
    if (((validMask & 2u) != 0u) && (t.y < bestT)) {
        bestT = t.y;
        bestSlot = 1u;
    }
    if (((validMask & 4u) != 0u) && (t.z < bestT)) {
        bestT = t.z;
        bestSlot = 2u;
    }
    if (((validMask & 8u) != 0u) && (t.w < bestT)) {
        bestSlot = 3u;
    }
    return bestSlot;
}

fn select_farthest4(t: vec4<f32>, validMask: u32) -> u32 {
    var bestSlot = INVALID_INDEX;
    var bestT = -1.0;
    if (((validMask & 1u) != 0u) && (t.x > bestT)) {
        bestT = t.x;
        bestSlot = 0u;
    }
    if (((validMask & 2u) != 0u) && (t.y > bestT)) {
        bestT = t.y;
        bestSlot = 1u;
    }
    if (((validMask & 4u) != 0u) && (t.z > bestT)) {
        bestT = t.z;
        bestSlot = 2u;
    }
    if (((validMask & 8u) != 0u) && (t.w > bestT)) {
        bestSlot = 3u;
    }
    return bestSlot;
}

fn bvh4_stack_item_from_slot(childT: vec4<f32>, childKind: vec4<u32>, childIndex: vec4<u32>, slot: u32) -> Bvh4StackItem {
    switch slot {
        case 0u: { return Bvh4StackItem(childIndex.x, bitcast<u32>(childT.x), childKind.x, 0u); }
        case 1u: { return Bvh4StackItem(childIndex.y, bitcast<u32>(childT.y), childKind.y, 0u); }
        case 2u: { return Bvh4StackItem(childIndex.z, bitcast<u32>(childT.z), childKind.z, 0u); }
        default: { return Bvh4StackItem(childIndex.w, bitcast<u32>(childT.w), childKind.w, 0u); }
    }
}

fn push_bvh4_stack_entry(
    traversalStack: ptr<function, array<Bvh4StackItem, TLAS_TRAVERSAL_STACK_SIZE>>,
    stackPtr: ptr<function, u32>,
    item: Bvh4StackItem) -> bool {
    if (*stackPtr) >= TLAS_TRAVERSAL_STACK_SIZE {
        trace_stat_add_local_stack_spill(1u);
        return false;
    }
    (*traversalStack)[(*stackPtr)] = item;
    (*stackPtr) += 1u;
    return true;
}

fn trace_tlas_bvh4_local(rayOrigin: vec3<f32>, rayDirection: vec3<f32>, state: ptr<function, HitState>) {
    if uniforms.tlasNodeCount == 0u {
        return;
    }

    let invdir = safe_inverse_direction(rayDirection);
    var traversalStack: array<Bvh4StackItem, TLAS_TRAVERSAL_STACK_SIZE>;
    var stackPtr = 0u;
    var current = Bvh4StackItem(0u, bitcast<u32>(0.0), 2u, 0u);
    let earlyOut = traversal_early_out_enabled();
    let earlyOutBias = max(uniforms.traceControl.y, 0.0);

    loop {
        let currentT = bitcast<f32>(current.tBits);
        if should_skip_group_fast(currentT, (*state).bestT, earlyOut, earlyOutBias) {
            current.kind = 0u;
        }

        if current.kind == 1u {
            trace_instance_blas_local(rayOrigin, rayDirection, current.index, currentT, vec2<u32>(0u, 0x80000000u), state);
            current.kind = 0u;
        } else if current.kind == 2u {
            (*state).iterationCount += 1u;
            let node = tlasBvh4Nodes[current.index];
            let tx0 = (node.childMinX - vec4<f32>(rayOrigin.x)) * vec4<f32>(invdir.x);
            let tx1 = (node.childMaxX - vec4<f32>(rayOrigin.x)) * vec4<f32>(invdir.x);
            let ty0 = (node.childMinY - vec4<f32>(rayOrigin.y)) * vec4<f32>(invdir.y);
            let ty1 = (node.childMaxY - vec4<f32>(rayOrigin.y)) * vec4<f32>(invdir.y);
            let tz0 = (node.childMinZ - vec4<f32>(rayOrigin.z)) * vec4<f32>(invdir.z);
            let tz1 = (node.childMaxZ - vec4<f32>(rayOrigin.z)) * vec4<f32>(invdir.z);
            let tNear = max(max(min(tx0, tx1), min(ty0, ty1)), min(tz0, tz1));
            let tFar = min(min(max(tx0, tx1), max(ty0, ty1)), max(tz0, tz1));
            let nearT = max(tNear, vec4<f32>(uniforms.tMin));
            let farT = min(tFar, vec4<f32>(min((*state).bestT, uniforms.tMax)));

            var childT = nearT;
            var childKind = node.childMeta;
            var childIndex = vec4<u32>(INVALID_INDEX);
            var validMask = 0u;
            if ((node.childMeta.x != 0u) && (nearT.x <= farT.x)) {
                childIndex.x = node.childIndex.x;
                validMask |= 1u;
            }
            if ((node.childMeta.y != 0u) && (nearT.y <= farT.y)) {
                childIndex.y = node.childIndex.y;
                validMask |= 2u;
            }
            if ((node.childMeta.z != 0u) && (nearT.z <= farT.z)) {
                childIndex.z = node.childIndex.z;
                validMask |= 4u;
            }
            if ((node.childMeta.w != 0u) && (nearT.w <= farT.w)) {
                childIndex.w = node.childIndex.w;
                validMask |= 8u;
            }

            let nearestSlot = select_nearest4(childT, validMask);
            current.kind = 0u;

            var pushMask = validMask;
            if nearestSlot != INVALID_INDEX {
                pushMask &= ~(1u << nearestSlot);
            }
            for (var pushOrder = 0u; pushOrder < 3u; pushOrder += 1u) {
                let farthestSlot = select_farthest4(childT, pushMask);
                if farthestSlot == INVALID_INDEX {
                    break;
                }
                if !push_bvh4_stack_entry(&traversalStack, &stackPtr, bvh4_stack_item_from_slot(childT, childKind, childIndex, farthestSlot)) {
                    return;
                }
                pushMask &= ~(1u << farthestSlot);
            }

            if nearestSlot != INVALID_INDEX {
                current = bvh4_stack_item_from_slot(childT, childKind, childIndex, nearestSlot);
            }
        }

        if current.kind == 0u {
            var foundStackEntry = false;
            loop {
                if stackPtr == 0u {
                    break;
                }
                stackPtr -= 1u;
                let entry = traversalStack[stackPtr];
                let entryMinT = bitcast<f32>(entry.tBits);
                if should_skip_group_fast(entryMinT, (*state).bestT, earlyOut, earlyOutBias) {
                    continue;
                }
                current = entry;
                foundStackEntry = true;
                break;
            }
            if !foundStackEntry {
                return;
            }
        }
    }
}
#endif

fn trace_ray(rayId: u32) {
    if rayId >= uniforms.rayCount {
        return;
    }

    trace_stat_begin_ray();
    let ray = rayInput[rayId];
    if !trace_ray_is_valid(ray) {
        trace_stat_add_invalid_ray();
        if debug_mode_heatmap() {
            write_hit_output(rayId, DEBUG_HIT_INSTANCE_INVALID_RAY, 0u, 0.0, vec2<f32>(0.0), INVALID_INDEX, 0u);
        } else {
            write_hit_output(rayId, INVALID_INDEX, INVALID_INDEX, 0.0, vec2<f32>(0.0), INVALID_INDEX, 0u);
        }
        return;
    }

    trace_stat_begin_valid_ray();
    var hitState = HitState(trace_ray_tmax(ray), INVALID_INDEX, INVALID_INDEX, vec2<f32>(0.0), 0u);
#ifdef SWRT_WORLD_CWBVH
    trace_world_cwbvh(trace_ray_origin(ray), trace_ray_direction(ray), &hitState);
#elif defined(SWRT_TLAS_BVH4)
    trace_tlas_bvh4_local(trace_ray_origin(ray), trace_ray_direction(ray), &hitState);
#else
    trace_tlas_local(trace_ray_origin(ray), trace_ray_direction(ray), &hitState);
#endif
    store_trace_result(rayId, hitState);
}
