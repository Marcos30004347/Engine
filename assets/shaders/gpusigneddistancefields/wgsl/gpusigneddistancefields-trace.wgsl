#include "gpusigneddistancefields_common.wgsl"

struct TraceUniforms {
    rayCount : u32,
    rayOffset : u32,
    hitOffset : u32,
    traceStatsEnabled : u32,
    maxSteps : u32,
    atlasBricksPerAxis : u32,
    cascadeCount : u32,
    cascadeOverride : i32,
    surfaceCachePageResolution : u32,
    surfaceCachePagesPerRow : u32,
    _padding0 : vec2<u32>,
    hitThresholdScale : f32,
    emptySpaceStepScale : f32,
    normalBias : f32,
    maxTraceDistance : f32,
}

struct TraceRayGPU {
    originAndMinT : vec4<f32>,
    directionAndMaxT : vec4<f32>,
    metadata : vec4<u32>,
}

struct TraceHitGPU {
    worldPositionAndDistance : vec4<f32>,
    worldNormalAndStepCount : vec4<f32>,
    localUVWAndAux : vec4<f32>,
    metadata : vec4<u32>,
    traceInfo : vec4<u32>,
    surfaceMaterialReference : vec4<u32>,
}

const TRACE_RAY_FLAG_ACTIVE : u32 = 1u << 0u;

const TRACE_HIT_FLAG_HIT : u32 = 1u << 0u;
const TRACE_HIT_FLAG_HAS_CASCADE : u32 = 1u << 1u;
const TRACE_HIT_FLAG_HAS_BRICK : u32 = 1u << 2u;
const TRACE_HIT_FLAG_HAS_UNINITIALIZED : u32 = 1u << 3u;
const TRACE_HIT_FLAG_HAS_VALID_BRICK_ID : u32 = 1u << 4u;
const TRACE_HIT_FLAG_HAS_ROOT_AABB : u32 = 1u << 5u;
const TRACE_HIT_FLAG_HAS_TREE_ROOT : u32 = 1u << 6u;
const TRACE_HIT_FLAG_HAS_MID_AABB : u32 = 1u << 7u;
const TRACE_HIT_FLAG_HAS_BOTTOM_AABB : u32 = 1u << 8u;
const TRACE_HIT_FLAG_INVALID_DIRECTION : u32 = 1u << 9u;
const TRACE_HIT_FLAG_INACTIVE_RAY : u32 = 1u << 10u;

const TRACE_STATS_COUNTER_TOTAL_RAYS : u32 = 0u;
const TRACE_STATS_COUNTER_ACTIVE_RAYS : u32 = 1u;
const TRACE_STATS_COUNTER_INACTIVE_RAYS : u32 = 2u;
const TRACE_STATS_COUNTER_INVALID_DIRECTION_RAYS : u32 = 3u;
const TRACE_STATS_COUNTER_HIT_RAYS : u32 = 4u;
const TRACE_STATS_COUNTER_MISS_RAYS : u32 = 5u;
const TRACE_STATS_COUNTER_HAS_CASCADE_RAYS : u32 = 6u;
const TRACE_STATS_COUNTER_HAS_VALID_BRICK_RAYS : u32 = 7u;
const TRACE_STATS_COUNTER_HAS_UNINITIALIZED_RAYS : u32 = 8u;
const TRACE_STATS_COUNTER_HAS_ROOT_AABB_RAYS : u32 = 9u;
const TRACE_STATS_COUNTER_HAS_TREE_ROOT_RAYS : u32 = 10u;
const TRACE_STATS_COUNTER_HAS_MID_AABB_RAYS : u32 = 11u;
const TRACE_STATS_COUNTER_HAS_BOTTOM_AABB_RAYS : u32 = 12u;
const TRACE_STATS_COUNTER_FAILED_CASCADE_RAYS : u32 = 13u;
const TRACE_STATS_COUNTER_FAILED_ROOT_RAYS : u32 = 14u;
const TRACE_STATS_COUNTER_FAILED_MID_RAYS : u32 = 15u;
const TRACE_STATS_COUNTER_FAILED_BOTTOM_RAYS : u32 = 16u;
const TRACE_STATS_COUNTER_FAILED_BRICK_RAYS : u32 = 17u;
const TRACE_STATS_COUNTER_FAILED_UNINITIALIZED_RAYS : u32 = 18u;
const TRACE_STATS_COUNTER_COUNT : u32 = 19u;

@group(0) @binding(0) var<uniform> gTraceUniforms : TraceUniforms;
@group(0) @binding(1) var<storage, read> cascadeInfos : array<CascadeInfoGPU>;
@group(0) @binding(2) var<storage, read> traceRays : array<TraceRayGPU>;
@group(0) @binding(3) var<storage, read_write> traceHits : array<TraceHitGPU>;
@group(0) @binding(4) var<storage, read> combinedBrickMap0 : array<u32>;
@group(0) @binding(5) var<storage, read> combinedBrickMap1 : array<u32>;
@group(0) @binding(6) var<storage, read> combinedBrickMap2 : array<u32>;
@group(0) @binding(7) var<storage, read> combinedBrickMap3 : array<u32>;
@group(0) @binding(8) var<storage, read> combinedBrickMap4 : array<u32>;
@group(0) @binding(9) var<storage, read> combinedBrickMap5 : array<u32>;
@group(0) @binding(10) var<storage, read> combinedBrickMap6 : array<u32>;
@group(0) @binding(11) var<storage, read> combinedBrickMap7 : array<u32>;
@group(0) @binding(12) var sdfAtlasTexture : texture_3d<f32>;
@group(0) @binding(13) var sdfAtlasSampler : sampler;
@group(0) @binding(14) var<storage, read> contextBrickAabb : array<u32>;
@group(0) @binding(15) var<storage, read> combinedAabbTree0 : array<u32>;
@group(0) @binding(16) var<storage, read> combinedAabbTree1 : array<u32>;
@group(0) @binding(17) var<storage, read> combinedAabbTree2 : array<u32>;
@group(0) @binding(18) var<storage, read> combinedAabbTree3 : array<u32>;
@group(0) @binding(19) var<storage, read> combinedAabbTree4 : array<u32>;
@group(0) @binding(20) var<storage, read> combinedAabbTree5 : array<u32>;
@group(0) @binding(21) var<storage, read> combinedAabbTree6 : array<u32>;
@group(0) @binding(22) var<storage, read> combinedAabbTree7 : array<u32>;
@group(0) @binding(23) var<storage, read_write> traceStats : array<atomic<u32>, TRACE_STATS_COUNTER_COUNT>;
@group(0) @binding(26) var surfaceCacheMaterialReferenceAtlas : texture_2d<u32>;
@group(0) @binding(27) var surfaceCacheMaterialReferenceBasisAtlas : texture_2d<u32>;

#include "gpusigneddistancefields-trace-common.wgsl"

fn make_empty_material_reference() -> vec4<u32> {
    return vec4<u32>(0xffffffffu, 0u, 0u, 0u);
}

fn material_reference_valid(reference: vec4<u32>) -> bool {
    return (reference.w & (1u << 31u)) != 0u;
}

fn material_reference_uv(reference: vec4<u32>) -> vec2<f32> {
    return vec2<f32>(bitcast<f32>(reference.y), bitcast<f32>(reference.z));
}

fn material_reference_uv_window(reference: vec4<u32>) -> vec2<f32> {
    let qx = reference.w & 2047u;
    let qy = (reference.w >> 11u) & 2047u;
    let signX = select(1.0, -1.0, (reference.w & (1u << 22u)) != 0u);
    let signY = select(1.0, -1.0, (reference.w & (1u << 23u)) != 0u);
    return vec2<f32>(f32(qx) * signX, f32(qy) * signY) / 2047.0;
}

fn pack_material_reference_uv_window(uvWindowSize: vec2<f32>) -> u32 {
    let width = clamp(u32(ceil(clamp(abs(uvWindowSize.x), 0.0, 1.0) * 2047.0)), 1u, 2047u);
    let height = clamp(u32(ceil(clamp(abs(uvWindowSize.y), 0.0, 1.0) * 2047.0)), 1u, 2047u);
    let signX = select(0u, 1u << 22u, uvWindowSize.x < 0.0);
    let signY = select(0u, 1u << 23u, uvWindowSize.y < 0.0);
    return width | (height << 11u) | signX | signY | (1u << 31u);
}

fn pack_material_reference(materialId: u32, uv: vec2<f32>, uvWindowSize: vec2<f32>) -> vec4<u32> {
    return vec4<u32>(materialId, bitcast<u32>(uv.x), bitcast<u32>(uv.y), pack_material_reference_uv_window(uvWindowSize));
}

fn unwrap_material_reference_uv(baseUv: vec2<f32>, uv: vec2<f32>) -> vec2<f32> {
    let delta = uv - baseUv;
    return baseUv + delta - round(delta);
}

fn load_surface_cache_material_reference(brickId: u32, faceIndex: u32, pageTexel: vec2<u32>) -> vec4<u32> {
    let atlasTexel = GPUSignedDistanceFieldsSurfaceCacheTexel(
        brickId,
        faceIndex,
        pageTexel,
        gTraceUniforms.surfaceCachePageResolution,
        gTraceUniforms.surfaceCachePagesPerRow);
    return textureLoad(surfaceCacheMaterialReferenceAtlas, vec2<i32>(atlasTexel), 0);
}

fn load_surface_cache_material_reference_basis(brickId: u32, faceIndex: u32, pageTexel: vec2<u32>) -> vec4<f32> {
    let atlasTexel = GPUSignedDistanceFieldsSurfaceCacheTexel(
        brickId,
        faceIndex,
        pageTexel,
        gTraceUniforms.surfaceCachePageResolution,
        gTraceUniforms.surfaceCachePagesPerRow);
    let packed = textureLoad(surfaceCacheMaterialReferenceBasisAtlas, vec2<i32>(atlasTexel), 0);
    return vec4<f32>(bitcast<f32>(packed.x), bitcast<f32>(packed.y), bitcast<f32>(packed.z), bitcast<f32>(packed.w));
}

fn material_reference_at_hit_uv(reference: vec4<u32>, uvBasis: vec4<f32>, faceUv: vec2<f32>, pageTexel: vec2<u32>, pageResolution: u32) -> vec4<u32> {
    if (!material_reference_valid(reference)) {
        return reference;
    }
    let uvRangeOrigin = material_reference_uv(reference);
    let texelLocal = clamp(faceUv * f32(pageResolution) - vec2<f32>(pageTexel), vec2<f32>(0.0), vec2<f32>(1.0));
    let uv = uvRangeOrigin + uvBasis.xy * texelLocal.x + uvBasis.zw * texelLocal.y;
    return vec4<u32>(reference.x, bitcast<u32>(uv.x), bitcast<u32>(uv.y), reference.w);
}

fn filter_surface_cache_material_reference(
    r00: vec4<u32>,
    r10: vec4<u32>,
    r01: vec4<u32>,
    r11: vec4<u32>,
    texelFract: vec2<f32>) -> vec4<u32> {
    if (!material_reference_valid(r00) ||
        !material_reference_valid(r10) ||
        !material_reference_valid(r01) ||
        !material_reference_valid(r11) ||
        r00.x != r10.x ||
        r00.x != r01.x ||
        r00.x != r11.x) {
        return make_empty_material_reference();
    }

    let uv00 = material_reference_uv(r00);
    let uv10 = unwrap_material_reference_uv(uv00, material_reference_uv(r10));
    let uv01 = unwrap_material_reference_uv(uv00, material_reference_uv(r01));
    let uv11 = unwrap_material_reference_uv(uv00, material_reference_uv(r11));
    let uv = mix(mix(uv00, uv10, texelFract.x), mix(uv01, uv11, texelFract.x), texelFract.y);

    let window00 = material_reference_uv_window(r00);
    let window10 = material_reference_uv_window(r10);
    let window01 = material_reference_uv_window(r01);
    let window11 = material_reference_uv_window(r11);
    let uvWindow = mix(mix(window00, window10, texelFract.x), mix(window01, window11, texelFract.x), texelFract.y);
    return pack_material_reference(r00.x, uv, uvWindow);
}

fn sample_surface_cache_face_material_reference(hit: TraceHit, faceIndex: u32) -> vec4<u32> {
    let pageResolution = gTraceUniforms.surfaceCachePageResolution;
    let faceUv = clamp(GPUSignedDistanceFieldsSurfaceCacheFaceUv(faceIndex, hit.uvw), vec2<f32>(0.0), vec2<f32>(0.999999));
    let pageTexel = min(vec2<u32>(faceUv * f32(pageResolution)), vec2<u32>(pageResolution - 1u));
    let direct = load_surface_cache_material_reference(hit.brickId, faceIndex, pageTexel);
    if (material_reference_valid(direct)) {
        let uvBasis = load_surface_cache_material_reference_basis(hit.brickId, faceIndex, pageTexel);
        return material_reference_at_hit_uv(direct, uvBasis, faceUv, pageTexel, pageResolution);
    }

    let maxTexel = i32(pageResolution) - 1i;
    let center = vec2<i32>(pageTexel);
    let maxRadius = min(2i, maxTexel);
    for (var radius = 1i; radius <= maxRadius; radius = radius + 1i) {
        for (var y = -radius; y <= radius; y = y + 1i) {
            for (var x = -radius; x <= radius; x = x + 1i) {
                let texel = center + vec2<i32>(x, y);
                if (all(texel >= vec2<i32>(0i)) && all(texel <= vec2<i32>(maxTexel))) {
                    let reference = load_surface_cache_material_reference(hit.brickId, faceIndex, vec2<u32>(texel));
                    if (material_reference_valid(reference)) {
                        let uvBasis = load_surface_cache_material_reference_basis(hit.brickId, faceIndex, vec2<u32>(texel));
                        return material_reference_at_hit_uv(reference, uvBasis, faceUv, vec2<u32>(texel), pageResolution);
                    }
                }
            }
        }
    }

    return direct;
}

fn surface_cache_signed_face(axis: u32, selectorNormal: vec3<f32>) -> u32 {
    if (axis == 0u) {
        return select(GPUSignedDistanceFields_SurfaceCacheFace_NegX, GPUSignedDistanceFields_SurfaceCacheFace_PosX, selectorNormal.x >= 0.0);
    }
    if (axis == 1u) {
        return select(GPUSignedDistanceFields_SurfaceCacheFace_NegY, GPUSignedDistanceFields_SurfaceCacheFace_PosY, selectorNormal.y >= 0.0);
    }
    return select(GPUSignedDistanceFields_SurfaceCacheFace_NegZ, GPUSignedDistanceFields_SurfaceCacheFace_PosZ, selectorNormal.z >= 0.0);
}

fn sample_sdf_brick_material_reference(hit: TraceHit, selectorNormal: vec3<f32>) -> vec4<u32> {
    if (gTraceUniforms.surfaceCachePageResolution == 0u || gTraceUniforms.surfaceCachePagesPerRow == 0u || !GPUSignedDistanceFieldsIsBrickValid(hit.brickId)) {
        return make_empty_material_reference();
    }

    let absNormal = abs(selectorNormal);
    var faceIndex = surface_cache_signed_face(2u, selectorNormal);
    if (absNormal.x >= absNormal.y && absNormal.x >= absNormal.z) {
        faceIndex = surface_cache_signed_face(0u, selectorNormal);
    } else if (absNormal.y >= absNormal.z) {
        faceIndex = surface_cache_signed_face(1u, selectorNormal);
    }

    let reference = sample_surface_cache_face_material_reference(hit, faceIndex);
    if (material_reference_valid(reference)) {
        return reference;
    }

    for (var fallbackFace = 0u; fallbackFace < GPUSignedDistanceFields_SurfaceCacheFaceCount; fallbackFace = fallbackFace + 1u) {
        if (fallbackFace == faceIndex) {
            continue;
        }
        let fallbackReference = sample_surface_cache_face_material_reference(hit, fallbackFace);
        if (material_reference_valid(fallbackReference)) {
            return fallbackReference;
        }
    }

    return make_empty_material_reference();
}

fn make_trace_parameters() -> TraceParameters {
    let fallbackCascadeCount = min(arrayLength(&cascadeInfos), GPUSignedDistanceFields_TraceMaxCascades);
    let cascadeCount = select(gTraceUniforms.cascadeCount, fallbackCascadeCount, gTraceUniforms.cascadeCount == 0u);
    let maxSteps = max(gTraceUniforms.maxSteps, 1u);
    let atlasBricksPerAxis = max(gTraceUniforms.atlasBricksPerAxis, 1u);
    let hitThresholdScale = select(gTraceUniforms.hitThresholdScale, 0.75, gTraceUniforms.hitThresholdScale <= 0.0);
    let emptySpaceStepScale = select(gTraceUniforms.emptySpaceStepScale, 1.0, gTraceUniforms.emptySpaceStepScale <= 0.0);
    return TraceParameters(
        maxSteps,
        atlasBricksPerAxis,
        cascadeCount,
        gTraceUniforms.cascadeOverride,
        hitThresholdScale,
        emptySpaceStepScale);
}

fn pack_trace_flags(hit: TraceHit) -> u32 {
    var flags = 0u;
    if (hit.hit) {
        flags |= TRACE_HIT_FLAG_HIT;
    }
    if (hit.hasCascade) {
        flags |= TRACE_HIT_FLAG_HAS_CASCADE;
    }
    if (hit.hasBrick) {
        flags |= TRACE_HIT_FLAG_HAS_BRICK;
    }
    if (hit.hasUninitialized) {
        flags |= TRACE_HIT_FLAG_HAS_UNINITIALIZED;
    }
    if (hit.hasValidBrickId) {
        flags |= TRACE_HIT_FLAG_HAS_VALID_BRICK_ID;
    }
    if (hit.hasRootAabbData) {
        flags |= TRACE_HIT_FLAG_HAS_ROOT_AABB;
    }
    if (hit.hasTreeRoot) {
        flags |= TRACE_HIT_FLAG_HAS_TREE_ROOT;
    }
    if (hit.hasMidAabb) {
        flags |= TRACE_HIT_FLAG_HAS_MID_AABB;
    }
    if (hit.hasBottomAabb) {
        flags |= TRACE_HIT_FLAG_HAS_BOTTOM_AABB;
    }
    return flags;
}

fn pack_cascade_and_flags(cascadeIndex: u32, flags: u32) -> u32 {
    return (cascadeIndex & 0xffu) | ((flags & 0x00ffffffu) << 8u);
}

fn make_miss_hit(ray: TraceRayGPU, extraFlags: u32) -> TraceHitGPU {
    return TraceHitGPU(
        vec4<f32>(0.0, 0.0, 0.0, -1.0),
        vec4<f32>(0.0, 0.0, 1.0, 0.0),
        vec4<f32>(0.0, 0.0, 0.0, -1.0),
        ray.metadata,
        vec4<u32>(pack_cascade_and_flags(0u, extraFlags), GPUSignedDistanceFields_InvalidId, 0u, 0u),
        make_empty_material_reference());
}

fn extract_trace_flags(hit : TraceHitGPU) -> u32 {
    return hit.traceInfo.x >> 8u;
}

fn accumulate_trace_stats(hit : TraceHitGPU) {
    if (gTraceUniforms.traceStatsEnabled == 0u) {
        return;
    }

    let hitFlags = extract_trace_flags(hit);
    let hitActive = (hitFlags & TRACE_HIT_FLAG_INACTIVE_RAY) == 0u;
    let hitValidDirection = (hitFlags & TRACE_HIT_FLAG_INVALID_DIRECTION) == 0u;
    let hitSucceeded = (hitFlags & TRACE_HIT_FLAG_HIT) != 0u;

    atomicAdd(&traceStats[TRACE_STATS_COUNTER_TOTAL_RAYS], 1u);
    if (hitActive) {
        atomicAdd(&traceStats[TRACE_STATS_COUNTER_ACTIVE_RAYS], 1u);
    } else {
        atomicAdd(&traceStats[TRACE_STATS_COUNTER_INACTIVE_RAYS], 1u);
    }
    if (!hitValidDirection) {
        atomicAdd(&traceStats[TRACE_STATS_COUNTER_INVALID_DIRECTION_RAYS], 1u);
    }
    if (hitSucceeded) {
        atomicAdd(&traceStats[TRACE_STATS_COUNTER_HIT_RAYS], 1u);
    } else if (hitActive && hitValidDirection) {
        atomicAdd(&traceStats[TRACE_STATS_COUNTER_MISS_RAYS], 1u);
    }
    if ((hitFlags & TRACE_HIT_FLAG_HAS_CASCADE) != 0u) {
        atomicAdd(&traceStats[TRACE_STATS_COUNTER_HAS_CASCADE_RAYS], 1u);
    }
    if ((hitFlags & TRACE_HIT_FLAG_HAS_VALID_BRICK_ID) != 0u) {
        atomicAdd(&traceStats[TRACE_STATS_COUNTER_HAS_VALID_BRICK_RAYS], 1u);
    }
    if ((hitFlags & TRACE_HIT_FLAG_HAS_UNINITIALIZED) != 0u) {
        atomicAdd(&traceStats[TRACE_STATS_COUNTER_HAS_UNINITIALIZED_RAYS], 1u);
    }
    if ((hitFlags & TRACE_HIT_FLAG_HAS_ROOT_AABB) != 0u) {
        atomicAdd(&traceStats[TRACE_STATS_COUNTER_HAS_ROOT_AABB_RAYS], 1u);
    }
    if ((hitFlags & TRACE_HIT_FLAG_HAS_TREE_ROOT) != 0u) {
        atomicAdd(&traceStats[TRACE_STATS_COUNTER_HAS_TREE_ROOT_RAYS], 1u);
    }
    if ((hitFlags & TRACE_HIT_FLAG_HAS_MID_AABB) != 0u) {
        atomicAdd(&traceStats[TRACE_STATS_COUNTER_HAS_MID_AABB_RAYS], 1u);
    }
    if ((hitFlags & TRACE_HIT_FLAG_HAS_BOTTOM_AABB) != 0u) {
        atomicAdd(&traceStats[TRACE_STATS_COUNTER_HAS_BOTTOM_AABB_RAYS], 1u);
    }

    if (!(hitActive && hitValidDirection) || hitSucceeded) {
        return;
    }

    if ((hitFlags & TRACE_HIT_FLAG_HAS_UNINITIALIZED) != 0u) {
        atomicAdd(&traceStats[TRACE_STATS_COUNTER_FAILED_UNINITIALIZED_RAYS], 1u);
    } else if ((hitFlags & TRACE_HIT_FLAG_HAS_CASCADE) == 0u) {
        atomicAdd(&traceStats[TRACE_STATS_COUNTER_FAILED_CASCADE_RAYS], 1u);
    } else if ((hitFlags & TRACE_HIT_FLAG_HAS_TREE_ROOT) == 0u) {
        atomicAdd(&traceStats[TRACE_STATS_COUNTER_FAILED_ROOT_RAYS], 1u);
    } else if ((hitFlags & TRACE_HIT_FLAG_HAS_MID_AABB) == 0u) {
        atomicAdd(&traceStats[TRACE_STATS_COUNTER_FAILED_MID_RAYS], 1u);
    } else if ((hitFlags & TRACE_HIT_FLAG_HAS_BOTTOM_AABB) == 0u) {
        atomicAdd(&traceStats[TRACE_STATS_COUNTER_FAILED_BOTTOM_RAYS], 1u);
    } else if ((hitFlags & TRACE_HIT_FLAG_HAS_VALID_BRICK_ID) == 0u) {
        atomicAdd(&traceStats[TRACE_STATS_COUNTER_FAILED_BRICK_RAYS], 1u);
    }
}

@compute @workgroup_size(64, 1, 1)
fn cs_main(@builtin(global_invocation_id) globalInvocationId : vec3<u32>) {
    let rayIndex = globalInvocationId.x;
    let rayCount = gTraceUniforms.rayCount;
    if (rayIndex >= rayCount) {
        return;
    }

    let traceRayIndex = gTraceUniforms.rayOffset + rayIndex;
    let traceHitIndex = gTraceUniforms.hitOffset + rayIndex;
    let ray = traceRays[traceRayIndex];
    if ((ray.metadata.z & TRACE_RAY_FLAG_ACTIVE) == 0u) {
        let missHit = make_miss_hit(ray, TRACE_HIT_FLAG_INACTIVE_RAY);
        traceHits[traceHitIndex] = missHit;
        accumulate_trace_stats(missHit);
        return;
    }

    let directionLength = length(ray.directionAndMaxT.xyz);
    if (directionLength <= 1.0e-6) {
        let missHit = make_miss_hit(ray, TRACE_HIT_FLAG_INVALID_DIRECTION);
        traceHits[traceHitIndex] = missHit;
        accumulate_trace_stats(missHit);
        return;
    }

    let rayDirection = ray.directionAndMaxT.xyz / directionLength;
    let rayMinT = max(ray.originAndMinT.w, max(gTraceUniforms.normalBias, 0.0));
    let maxTraceDistance = select(gTraceUniforms.maxTraceDistance, ray.directionAndMaxT.w, gTraceUniforms.maxTraceDistance <= 0.0);
    let rayMaxT = min(ray.directionAndMaxT.w, maxTraceDistance);
    if (rayMaxT <= rayMinT) {
        let missHit = make_miss_hit(ray, 0u);
        traceHits[traceHitIndex] = missHit;
        accumulate_trace_stats(missHit);
        return;
    }

    let traceParams = make_trace_parameters();
    let hit = trace_brixelizer_raw(ray.originAndMinT.xyz, rayDirection, rayMinT, rayMaxT, traceParams);
    let hitFlags = pack_trace_flags(hit);
    let normal = select(vec3<f32>(0.0, 0.0, 1.0), estimate_hit_normal(hit, rayDirection, traceParams), hit.hit);
    var materialReference = make_empty_material_reference();
    if (hit.hit) {
        materialReference = sample_sdf_brick_material_reference(hit, normal);
    }
    let traceHit = TraceHitGPU(
        vec4<f32>(hit.worldPosition, select(-1.0, hit.t, hit.hit)),
        vec4<f32>(normal, f32(hit.stepCount)),
        vec4<f32>(hit.uvw, hit.firstBrickDistance),
        ray.metadata,
        vec4<u32>(pack_cascade_and_flags(hit.cascadeIndex, hitFlags), hit.brickId, hit.stepCount, 0u),
        materialReference);
    traceHits[traceHitIndex] = traceHit;
    accumulate_trace_stats(traceHit);
}
