#include "globalIllumination/wgsl/trace-shading-types.wgsl"

struct ScreenSpaceHitShadingResolveUniforms {
    enabled : u32,
    hitCapacity : u32,
    _padding0 : u32,
    _padding1 : u32,
};

struct ScreenHit {
    origin : vec4<f32>,
    direction : vec4<f32>,
    hitPositionDistance : vec4<f32>,
    pixelsAndRay : vec4<u32>,
};

struct ScreenHitBuffer {
    count : atomic<u32>,
    overflow : atomic<u32>,
    capacity : u32,
    totalHits : atomic<u32>,
    _padding0 : u32,
    _padding1 : u32,
    _padding2 : u32,
    _padding3 : u32,
    hits : array<ScreenHit>,
};

@group(0) @binding(0) var<uniform> uniforms : ScreenSpaceHitShadingResolveUniforms;
@group(0) @binding(1) var<storage, read> screenHits : ScreenHitBuffer;
@group(0) @binding(2) var<storage, read_write> traceShading : array<TraceShadingParameters>;
@group(0) @binding(3) var geometryNormalTexture : texture_2d<f32>;
@group(0) @binding(4) var baseColorTexture : texture_2d<f32>;
@group(0) @binding(5) var normalTexture : texture_2d<f32>;
@group(0) @binding(6) var materialIdTexture : texture_2d<u32>;
@group(0) @binding(7) var materialUVTexture : texture_2d<f32>;

fn safe_normalize(value : vec3<f32>, fallback : vec3<f32>) -> vec3<f32> {
    let len = length(value);
    return select(fallback, value / len, len > 1.0e-6);
}

fn clamp_pixel(pixel : vec2<u32>, dimensions : vec2<u32>) -> vec2<i32> {
    return vec2<i32>(min(pixel, dimensions - vec2<u32>(1u, 1u)));
}

@compute @workgroup_size(64, 1, 1)
fn cs_main(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let rayId = globalId.x;
    if (rayId >= max(uniforms.hitCapacity, 1u)) {
        return;
    }

    if (uniforms.enabled == 0u || rayId >= atomicLoad(&screenHits.count)) {
        traceShading[rayId] = invalid_trace_shading_parameters();
        return;
    }

    let hit = screenHits.hits[rayId];
    let colorDims = textureDimensions(baseColorTexture);
    let normalDims = textureDimensions(normalTexture);
    let geometryNormalDims = textureDimensions(geometryNormalTexture);
    let materialIdDims = textureDimensions(materialIdTexture);
    let materialUVDims = textureDimensions(materialUVTexture);
    let receiverPixel = hit.pixelsAndRay.xy;
    let hitPixel = hit.pixelsAndRay.zw;
    let clampedHitColorPixel = clamp_pixel(hitPixel, colorDims);
    let clampedReceiverColorPixel = clamp_pixel(receiverPixel, colorDims);
    let clampedHitNormalPixel = clamp_pixel(hitPixel, normalDims);
    let clampedReceiverNormalPixel = clamp_pixel(receiverPixel, geometryNormalDims);
    let hitGeometryNormalPixel = clamp_pixel(hitPixel, geometryNormalDims);
    let materialIdPixel = clamp_pixel(hitPixel, materialIdDims);
    let materialUVPixel = clamp_pixel(hitPixel, materialUVDims);

    let hitAlbedo = textureLoad(baseColorTexture, clampedHitColorPixel, 0);
    let receiverAlbedo = textureLoad(baseColorTexture, clampedReceiverColorPixel, 0);
    let hitGeometryNormal = safe_normalize(textureLoad(geometryNormalTexture, hitGeometryNormalPixel, 0).xyz, vec3<f32>(0.0, 1.0, 0.0));
    let hitShadingNormal = safe_normalize(textureLoad(normalTexture, clampedHitNormalPixel, 0).xyz, hitGeometryNormal);
    let receiverNormal = safe_normalize(textureLoad(geometryNormalTexture, clampedReceiverNormalPixel, 0).xyz, vec3<f32>(0.0, 1.0, 0.0));
    let materialId = textureLoad(materialIdTexture, materialIdPixel, 0).r;
    let materialUv = textureLoad(materialUVTexture, materialUVPixel, 0).xy;
    let flags = TRACE_SHADING_FLAG_VALID | TRACE_SHADING_FLAG_SURFACE_CACHE_MATERIAL | (TRACE_SHADING_SOURCE_SCREEN_SPACE << 24u);

    traceShading[rayId] = TraceShadingParameters(
        hit.hitPositionDistance,
        vec4<f32>(hitShadingNormal, 1.0),
        vec4<f32>(materialUv, 0.0, 0.0),
        vec4<f32>(hitAlbedo.rgb, hitAlbedo.a),
        vec4<f32>(hit.origin.xyz, 0.15915494),
        vec4<f32>(receiverNormal, 1.0),
        vec4<f32>(receiverAlbedo.rgb, receiverAlbedo.a),
        vec4<f32>(safe_normalize(hit.direction.xyz, vec3<f32>(0.0, 1.0, 0.0)), 0.0),
        vec4<u32>(materialId, 0xffffffffu, 0xffffffffu, flags));
}
