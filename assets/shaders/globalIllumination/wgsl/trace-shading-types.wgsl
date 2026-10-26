const TRACE_SHADING_SOURCE_NONE : u32 = 0u;
const TRACE_SHADING_SOURCE_SCREEN_SPACE : u32 = 1u;
const TRACE_SHADING_SOURCE_SDF : u32 = 2u;
const TRACE_SHADING_SOURCE_BVH : u32 = 3u;

const TRACE_SHADING_FLAG_VALID : u32 = 1u << 0u;
const TRACE_SHADING_FLAG_SURFACE_CACHE_MATERIAL : u32 = 1u << 1u;
const TRACE_SHADING_FLAG_FALLBACK_MATERIAL : u32 = 1u << 2u;
const TRACE_SHADING_FLAG_SCREEN_TRACE_MISS : u32 = 1u << 3u;

struct TraceShadingParameters {
    positionHitT : vec4<f32>,
    normalValidity : vec4<f32>,
    uvBarycentrics : vec4<f32>,
    albedo : vec4<f32>,
    receiverPositionPdf : vec4<f32>,
    receiverNormalValidity : vec4<f32>,
    receiverAlbedo : vec4<f32>,
    rayDirection : vec4<f32>,
    ids : vec4<u32>,
};

fn invalid_trace_shading_parameters() -> TraceShadingParameters {
    return TraceShadingParameters(
        vec4<f32>(0.0),
        vec4<f32>(0.0, 1.0, 0.0, 0.0),
        vec4<f32>(0.0),
        vec4<f32>(1.0, 1.0, 1.0, 0.0),
        vec4<f32>(0.0, 0.0, 0.0, 0.15915494),
        vec4<f32>(0.0, 1.0, 0.0, 0.0),
        vec4<f32>(1.0, 1.0, 1.0, 0.0),
        vec4<f32>(0.0, 1.0, 0.0, 0.0),
        vec4<u32>(0xffffffffu, 0xffffffffu, 0xffffffffu, 0u));
}
