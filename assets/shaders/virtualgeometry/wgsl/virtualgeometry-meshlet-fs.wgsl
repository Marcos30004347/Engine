#include "virtualgeometrydata.wgsl"

// Include sync marker for virtualgeometrydata.wgsl changes.

#ifdef OUTPUT_MOTION_VECTORS
@group(0) @binding(7) var<uniform> temporalUniforms : TemporalUniforms;
#endif

#ifdef COLLECT_FRAME_STATISTICS
@group(0) @binding(0) var<uniform> uniforms : CullingUniforms;
#ifdef OUTPUT_MOTION_VECTORS
@group(0) @binding(8) var<storage, read_write> frameStatistics : array<atomic<u32>>;
#else
@group(0) @binding(7) var<storage, read_write> frameStatistics : array<atomic<u32>>;
#endif
#endif

struct FragmentInput {
    @builtin(position) clipPos : vec4<f32>,
    @location(0) @interpolate(flat) instanceIndex : u32,
    @location(1) @interpolate(flat) globalCluster : u32,
    @location(2) @interpolate(flat) triangleId : u32,
    @location(3) @interpolate(flat) materialIndex : u32,
    @location(4) uv : vec2<f32>,
    @location(5) worldNormal : vec3<f32>,
#ifdef OUTPUT_MOTION_VECTORS
    @location(6) currentClip : vec4<f32>,
    @location(7) previousClip : vec4<f32>,
#endif
};

struct FragmentOutput {
    @location(0) packedGeometryIdsLo : u32,
    @location(1) packedGeometryIdsHi : u32,
    @location(2) materialId : u32,
    @location(3) materialUV : vec2<f32>,
    @location(4) geometryNormal : vec4<f32>,
#ifdef OUTPUT_MOTION_VECTORS
    @location(5) motionVectors : vec2<f32>,
#endif
};

#ifdef OUTPUT_MOTION_VECTORS
fn clipToUv(clipPos: vec4<f32>) -> vec2<f32> {
    let ndc = clipPos.xy / clipPos.w;
    return vec2<f32>(ndc.x * 0.5 + 0.5, ndc.y * 0.5 + 0.5);
}
#endif

/// @brief Fragment shader that writes the geometry identity and the material
/// ID/UV to the G-buffer targets. The identity is the full instance index in
/// the low word and the visibility payload - packVisibilityPayload(
/// globalCluster, triangleId), the same encoding the software rasterizer
/// emits - in the high word, so every consumer resolves geometry through
/// decodedClusterGeometryInfos[globalCluster] regardless of which rasterizer
/// produced the pixel.
@fragment
fn fs_main(in: FragmentInput) -> FragmentOutput {
    var out : FragmentOutput;

#ifdef COLLECT_FRAME_STATISTICS
    let pixelCoords = vec2<u32>(in.clipPos.xy);
    let pixelIndex = pixelCoords.y * uniforms.viewport.x + pixelCoords.x;
    if (pixelCoords.x < uniforms.viewport.x && pixelCoords.y < uniforms.viewport.y && pixelIndex < arrayLength(&frameStatistics)) {
        atomicAdd(&frameStatistics[pixelIndex], 1u);
    }
#endif

    out.packedGeometryIdsLo = in.instanceIndex;
    out.packedGeometryIdsHi = packVisibilityPayload(in.globalCluster, in.triangleId);
    out.materialId = in.materialIndex;
    out.materialUV = in.uv;
    let worldNormalLength = length(in.worldNormal);
    let worldNormal = select(vec3<f32>(0.0, 0.0, 1.0), in.worldNormal / worldNormalLength, worldNormalLength > 1e-6);
    out.geometryNormal = vec4<f32>(worldNormal, 1.0);
#ifdef OUTPUT_MOTION_VECTORS
    if (temporalUniforms.hasPreviousFrame == 0u || abs(in.previousClip.w) <= 1e-6 || abs(in.currentClip.w) <= 1e-6) {
        out.motionVectors = vec2<f32>(0.0, 0.0);
    } else {
        let currentUv = clipToUv(in.currentClip);
        let previousUv = clipToUv(in.previousClip);
        out.motionVectors = currentUv - previousUv;
    }
#endif

    return out;
}
