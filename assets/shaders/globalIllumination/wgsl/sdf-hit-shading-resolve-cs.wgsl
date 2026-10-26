#include "globalIllumination/wgsl/trace-shading-types.wgsl"

struct SDFHitShadingResolveUniforms {
    controls : vec4<u32>,
};

struct SdfTraceRay {
    originAndMinT : vec4<f32>,
    directionAndMaxT : vec4<f32>,
    metadata : vec4<u32>,
};

struct SdfTraceHit {
    worldPositionAndDistance : vec4<f32>,
    worldNormalAndStepCount : vec4<f32>,
    localUVWAndAux : vec4<f32>,
    metadata : vec4<u32>,
    traceInfo : vec4<u32>,
    surfaceMaterialReference : vec4<u32>,
};

struct FailedRay {
    origin : vec4<f32>,
    direction : vec4<f32>,
    pixelAndRay : vec4<u32>,
};

struct FailedRayBuffer {
    count : atomic<u32>,
    overflow : atomic<u32>,
    capacity : u32,
    totalRays : atomic<u32>,
    offscreenRays : atomic<u32>,
    _padding0 : u32,
    _padding1 : u32,
    _padding2 : u32,
    rays : array<FailedRay>,
};

const TRACE_HIT_FLAG_HIT : u32 = 1u << 0u;
const TRACE_HIT_FLAG_INACTIVE_RAY : u32 = 1u << 10u;
const TRACE_HIT_FLAG_INVALID_DIRECTION : u32 = 1u << 9u;

@group(0) @binding(0) var<uniform> uniforms : SDFHitShadingResolveUniforms;
@group(0) @binding(1) var<storage, read> sdfRayInput : array<SdfTraceRay>;
@group(0) @binding(2) var<storage, read> sdfHitOutput : array<SdfTraceHit>;
@group(0) @binding(3) var<storage, read> failedRays : FailedRayBuffer;
@group(0) @binding(4) var<storage, read_write> traceShading : array<TraceShadingParameters>;
@group(0) @binding(5) var geometryNormalTexture : texture_2d<f32>;
@group(0) @binding(6) var baseColorTexture : texture_2d<f32>;

fn safe_normalize(v : vec3<f32>, fallback : vec3<f32>) -> vec3<f32> {
    let len = length(v);
    return select(fallback, v / len, len > 1.0e-6);
}

fn extract_trace_flags(hit : SdfTraceHit) -> u32 {
    return hit.traceInfo.x >> 8u;
}

fn hit_is_valid(hit : SdfTraceHit) -> bool {
    let flags = extract_trace_flags(hit);
    return (flags & TRACE_HIT_FLAG_HIT) != 0u &&
        (flags & TRACE_HIT_FLAG_INACTIVE_RAY) == 0u &&
        (flags & TRACE_HIT_FLAG_INVALID_DIRECTION) == 0u &&
        hit.worldPositionAndDistance.w >= 0.0;
}

fn receiver_pixel(rayId : u32, ray : SdfTraceRay) -> vec2<i32> {
    if (ray.metadata.x != 0xffffffffu && ray.metadata.y != 0xffffffffu) {
        return vec2<i32>(ray.metadata.xy);
    }
    return vec2<i32>(failedRays.rays[rayId].pixelAndRay.xy);
}

@compute @workgroup_size(64, 1, 1)
fn cs_main(@builtin(global_invocation_id) gid : vec3<u32>) {
    let rayId = gid.x;
    if (rayId >= uniforms.controls.y || rayId >= arrayLength(&sdfRayInput) || rayId >= arrayLength(&sdfHitOutput) || rayId >= arrayLength(&traceShading)) {
        return;
    }

    if (uniforms.controls.x == 0u) {
        traceShading[rayId] = invalid_trace_shading_parameters();
        return;
    }

    let ray = sdfRayInput[rayId];
    let hit = sdfHitOutput[rayId];
    if (!hit_is_valid(hit)) {
        traceShading[rayId] = invalid_trace_shading_parameters();
        return;
    }

    let rayDir = safe_normalize(ray.directionAndMaxT.xyz, vec3<f32>(0.0, 1.0, 0.0));
    let pixel = receiver_pixel(rayId, ray);
    let receiverNormal = safe_normalize(textureLoad(geometryNormalTexture, pixel, 0).xyz, -rayDir);
    let receiverAlbedo = clamp(textureLoad(baseColorTexture, pixel, 0).rgb, vec3<f32>(0.0), vec3<f32>(8.0));
    let hitNormal = safe_normalize(hit.worldNormalAndStepCount.xyz, -rayDir);
    let materialReferenceValid = (hit.surfaceMaterialReference.w & (1u << 31u)) != 0u;
    let materialId = select(hit.traceInfo.y, hit.surfaceMaterialReference.x, materialReferenceValid);
    let materialUv = select(hit.localUVWAndAux.xy, vec2<f32>(bitcast<f32>(hit.surfaceMaterialReference.y), bitcast<f32>(hit.surfaceMaterialReference.z)), materialReferenceValid);
    var flags = TRACE_SHADING_FLAG_VALID | TRACE_SHADING_FLAG_SCREEN_TRACE_MISS | (TRACE_SHADING_SOURCE_SDF << 24u);
    if (materialReferenceValid) {
        flags |= TRACE_SHADING_FLAG_SURFACE_CACHE_MATERIAL;
    } else {
        flags |= TRACE_SHADING_FLAG_FALLBACK_MATERIAL;
    }

    traceShading[rayId] = TraceShadingParameters(
        vec4<f32>(hit.worldPositionAndDistance.xyz, hit.worldPositionAndDistance.w),
        vec4<f32>(hitNormal, 1.0),
        vec4<f32>(materialUv, hit.localUVWAndAux.z, hit.localUVWAndAux.w),
        vec4<f32>(receiverAlbedo, 1.0),
        vec4<f32>(ray.originAndMinT.xyz, 0.15915494),
        vec4<f32>(receiverNormal, 1.0),
        vec4<f32>(receiverAlbedo, 1.0),
        vec4<f32>(rayDir, 0.0),
        vec4<u32>(materialId, hit.traceInfo.z, ray.metadata.w, flags));
}
