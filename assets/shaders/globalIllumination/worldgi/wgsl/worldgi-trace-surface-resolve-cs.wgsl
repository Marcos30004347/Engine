#include "globalIllumination/wgsl/trace-shading-types.wgsl"
#include "globalIllumination/worldgi/wgsl/worldgi-types.wgsl"

struct WorldGITraceSurfaceResolveUniforms {
    enabled : u32,
    surfaceCount : u32,
    sourceOverride : u32,
    _padding0 : u32,
};

@group(0) @binding(0) var<uniform> uniforms : WorldGITraceSurfaceResolveUniforms;
@group(0) @binding(1) var<storage, read> traceShading : array<TraceShadingParameters>;
@group(0) @binding(2) var<storage, read_write> traceSurfaces : array<GITraceSurface>;

fn safe_normalize(value : vec3<f32>, fallback : vec3<f32>) -> vec3<f32> {
    let len = length(value);
    return select(fallback, value / len, len > 1.0e-6);
}

fn trace_source_to_gi_source(traceSource : u32) -> u32 {
    switch (traceSource) {
        case TRACE_SHADING_SOURCE_SCREEN_SPACE: {
            return GI_TRACE_SURFACE_SOURCE_SCREEN_TRACE;
        }
        case TRACE_SHADING_SOURCE_SDF: {
            return GI_TRACE_SURFACE_SOURCE_SDF;
        }
        case TRACE_SHADING_SOURCE_BVH: {
            return GI_TRACE_SURFACE_SOURCE_CWBVH;
        }
        default: {
            return GI_TRACE_SURFACE_SOURCE_INVALID;
        }
    }
}

@compute @workgroup_size(64, 1, 1)
fn cs_main(@builtin(global_invocation_id) gid : vec3<u32>) {
    let surfaceId = gid.x;
    if (surfaceId >= uniforms.surfaceCount || surfaceId >= arrayLength(&traceShading) || surfaceId >= arrayLength(&traceSurfaces)) {
        return;
    }

    if (uniforms.enabled == 0u) {
        traceSurfaces[surfaceId] = invalid_gi_trace_surface();
        return;
    }

    let shading = traceShading[surfaceId];
    let flags = shading.ids.w;
    if ((flags & TRACE_SHADING_FLAG_VALID) == 0u || shading.normalValidity.w <= 0.0) {
        traceSurfaces[surfaceId] = invalid_gi_trace_surface();
        return;
    }

    let traceSource = (flags >> 24u) & 0xffu;
    let resolvedSource = select(trace_source_to_gi_source(traceSource), uniforms.sourceOverride, uniforms.sourceOverride != 0u);
    let validFlags = flags & 0x00ffffffu;
    let confidence = clamp(shading.normalValidity.w, 0.0, 1.0);
    let rayDir = safe_normalize(shading.rayDirection.xyz, vec3<f32>(0.0, 1.0, 0.0));
    let viewDir = -rayDir;
    let normal = safe_normalize(shading.normalValidity.xyz, -rayDir);

    traceSurfaces[surfaceId] = GITraceSurface(
        shading.positionHitT,
        vec4<f32>(normal, confidence),
        vec4<f32>(normal, clamp(shading.albedo.a, 0.0, 1.0)),
        vec4<f32>(viewDir, 0.0),
        vec4<f32>(rayDir, shading.receiverPositionPdf.w),
        shading.uvBarycentrics,
        vec4<f32>(max(shading.albedo.rgb, vec3<f32>(0.0)), 1.0),
        vec4<f32>(0.0),
        shading.receiverPositionPdf,
        shading.receiverNormalValidity,
        shading.receiverAlbedo,
        shading.ids,
        vec4<u32>(resolvedSource, select(validFlags, validFlags | GI_TRACE_SURFACE_FLAG_VALID, confidence > 0.0), 0u, 0u));
}
