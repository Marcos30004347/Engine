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
    @location(1) @interpolate(flat) pageIndex : u32,
    @location(2) @interpolate(flat) groupIndex : u32,
    @location(3) @interpolate(flat) clusterIndex : u32,
    @location(4) @interpolate(flat) triangleId : u32,
    @location(5) @interpolate(flat) materialIndex : u32,
    @location(6) uv : vec2<f32>,
    @location(7) worldNormal : vec3<f32>,
#ifdef OUTPUT_MOTION_VECTORS
    @location(8) currentClip : vec4<f32>,
    @location(9) previousClip : vec4<f32>,
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

/// @brief Packs the instance index and the low 2 bits of the page index into a single u32 for the low geometry ID render target.
/// @param instanceIndex The index of the rendered instance (30-bit).
/// @param pageIndex The page index whose low 2 bits are packed into bits 30–31.
/// @returns A u32 combining instanceIndex and the low 2 bits of pageIndex.
fn packGeometryIdsLo(instanceIndex: u32, pageIndex: u32) -> u32 {
    return (instanceIndex & 0x3FFFFFFFu) | ((pageIndex & 0x3u) << 30u);
}

/// @brief Packs the upper page index bits, group index, cluster index, and triangle ID into a single u32 for the high geometry ID render target.
/// @param pageIndex The full page index; bits [17:2] are placed into bits [15:0].
/// @param groupIndex The within-page group index (6-bit), placed into bits [21:16].
/// @param clusterIndex The within-group cluster index (3-bit), placed into bits [24:22].
/// @param triangleId The local triangle ID (7-bit), placed into bits [31:25].
/// @returns A packed u32 encoding all four geometry identifiers.
fn packGeometryIdsHi(pageIndex: u32, groupIndex: u32, clusterIndex: u32, triangleId: u32) -> u32 {
    return ((pageIndex >> 2u) & 0xFFFFu)
        | ((groupIndex & 0x3Fu) << 16u)
        | ((clusterIndex & 0x7u) << 22u)
        | ((triangleId & 0x7Fu) << 25u);
}

/// @brief Fragment shader that writes packed geometry IDs (instance, page, group, cluster, triangle) and the material ID/UV to the G-buffer targets.
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

    out.packedGeometryIdsLo = packGeometryIdsLo(in.instanceIndex, in.pageIndex);
    out.packedGeometryIdsHi = packGeometryIdsHi(in.pageIndex, in.groupIndex, in.clusterIndex, in.triangleId);
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
