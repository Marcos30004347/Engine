const GI_TRACE_SURFACE_SOURCE_INVALID : u32 = 0u;
const GI_TRACE_SURFACE_SOURCE_PRIMARY_GBUFFER : u32 = 1u;
const GI_TRACE_SURFACE_SOURCE_SCREEN_TRACE : u32 = 2u;
const GI_TRACE_SURFACE_SOURCE_CWBVH : u32 = 3u;
const GI_TRACE_SURFACE_SOURCE_SDF : u32 = 4u;
const GI_TRACE_SURFACE_SOURCE_EMISSIVE_SAMPLE : u32 = 5u;
const GI_TRACE_SURFACE_SOURCE_LIGHT_SAMPLE : u32 = 6u;
const GI_TRACE_SURFACE_SOURCE_SKY_MISS : u32 = 7u;

const GI_TRACE_SURFACE_FLAG_VALID : u32 = 1u << 0u;
const GI_TRACE_SURFACE_FLAG_SURFACE_CACHE_MATERIAL : u32 = 1u << 1u;
const GI_TRACE_SURFACE_FLAG_FALLBACK_MATERIAL : u32 = 1u << 2u;
const GI_TRACE_SURFACE_FLAG_SCREEN_TRACE_MISS : u32 = 1u << 3u;

const WORLD_GI_RAY_TYPE_INVALID : u32 = 0u;
const WORLD_GI_RAY_TYPE_DIFFUSE : u32 = 1u;
const WORLD_GI_RAY_TYPE_GLOSSY : u32 = 2u;
const WORLD_GI_RAY_TYPE_FINAL_GATHER : u32 = 3u;

const WORLD_GI_RAY_BACKEND_NONE : u32 = 0u;
const WORLD_GI_RAY_BACKEND_SCREEN_TRACE : u32 = 1u << 0u;
const WORLD_GI_RAY_BACKEND_SDF : u32 = 1u << 1u;
const WORLD_GI_RAY_BACKEND_CWBVH : u32 = 1u << 2u;

const WORLD_GI_RAY_FLAG_VALID : u32 = 1u << 0u;

const WORLD_GI_LIGHT_TYPE_INVALID : u32 = 0u;
const WORLD_GI_LIGHT_TYPE_DIRECTIONAL : u32 = 1u;
const WORLD_GI_LIGHT_TYPE_POINT : u32 = 2u;
const WORLD_GI_LIGHT_TYPE_REFLECTION_PROBE : u32 = 3u;
const WORLD_GI_LIGHT_TYPE_DECAL : u32 = 4u;
const WORLD_GI_LIGHT_TYPE_EMISSIVE_RANGE : u32 = 5u;

const WORLD_GI_LIGHT_FLAG_ENABLED : u32 = 1u << 0u;

struct WorldGIRay {
    originMinT : vec4<f32>,
    directionMaxT : vec4<f32>,
    receiverPositionPdf : vec4<f32>,
    receiverNormalRoughness : vec4<f32>,
    receiverBaseColorMetallic : vec4<f32>,
    samplePdfMisWeight : vec4<f32>,
    pixelSample : vec4<u32>,
    typeBackendFlags : vec4<u32>,
};

fn invalid_world_gi_ray() -> WorldGIRay {
    return WorldGIRay(
        vec4<f32>(0.0),
        vec4<f32>(0.0),
        vec4<f32>(0.0, 0.0, 0.0, 0.15915494),
        vec4<f32>(0.0, 1.0, 0.0, 1.0),
        vec4<f32>(1.0, 1.0, 1.0, 0.0),
        vec4<f32>(0.15915494, 0.15915494, 1.0, 0.0),
        vec4<u32>(0u),
        vec4<u32>(WORLD_GI_RAY_TYPE_INVALID, WORLD_GI_RAY_BACKEND_NONE, 0u, 0u));
}

struct GITraceSurface {
    worldPositionHitT : vec4<f32>,
    geometryNormalConfidence : vec4<f32>,
    shadingNormalRoughness : vec4<f32>,
    viewDirectionMetallic : vec4<f32>,
    rayDirectionPdf : vec4<f32>,
    uvBarycentrics : vec4<f32>,
    baseColorOpacity : vec4<f32>,
    emissiveRadiance : vec4<f32>,
    receiverPositionPdf : vec4<f32>,
    receiverNormalValidity : vec4<f32>,
    receiverAlbedo : vec4<f32>,
    ids : vec4<u32>,
    sourceFlags : vec4<u32>,
};

fn invalid_gi_trace_surface() -> GITraceSurface {
    return GITraceSurface(
        vec4<f32>(0.0),
        vec4<f32>(0.0, 1.0, 0.0, 0.0),
        vec4<f32>(0.0, 1.0, 0.0, 1.0),
        vec4<f32>(0.0, 0.0, 1.0, 0.0),
        vec4<f32>(0.0, 1.0, 0.0, 0.15915494),
        vec4<f32>(0.0),
        vec4<f32>(1.0),
        vec4<f32>(0.0),
        vec4<f32>(0.0, 0.0, 0.0, 0.15915494),
        vec4<f32>(0.0, 1.0, 0.0, 0.0),
        vec4<f32>(1.0),
        vec4<u32>(0xffffffffu, 0xffffffffu, 0xffffffffu, 0u),
        vec4<u32>(GI_TRACE_SURFACE_SOURCE_INVALID, 0u, 0u, 0u));
}

struct WorldGILight {
    positionRange : vec4<f32>,
    directionType : vec4<f32>,
    colorIntensity : vec4<f32>,
    flags : vec4<u32>,
};

struct WorldGILightGridCell {
    itemCount : vec4<u32>,
};
