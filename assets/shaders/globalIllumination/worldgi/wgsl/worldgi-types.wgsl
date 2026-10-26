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
const WORLD_GI_LIGHT_TYPE_SPHERE : u32 = 6u;
const WORLD_GI_LIGHT_TYPE_PLANAR : u32 = 7u;
const WORLD_GI_LIGHT_TYPE_SKY : u32 = 8u;

const WORLD_GI_LIGHT_FLAG_ENABLED : u32 = 1u << 0u;
const WORLD_GI_LIGHT_FLAG_ATMOSPHERE_SH : u32 = 1u << 1u;

const WORLD_GI_LIGHT_BVH_NODE_TYPE_INVALID : u32 = 0u;
const WORLD_GI_LIGHT_BVH_NODE_TYPE_LEAF : u32 = 1u;
const WORLD_GI_LIGHT_BVH_NODE_TYPE_INTERNAL : u32 = 2u;
const WORLD_GI_LIGHT_BVH_NODE_TYPE_INFINITE : u32 = 3u;
const WORLD_GI_LIGHT_BVH_INVALID_NODE : u32 = 0xffffffffu;

const WORLD_GI_RADIANCE_CACHE_ENTRY_FLAG_EMPTY : u32 = 0u;
const WORLD_GI_RADIANCE_CACHE_ENTRY_FLAG_VALID : u32 = 1u << 0u;
const WORLD_GI_RADIANCE_CACHE_ENTRY_FLAG_LOCKED : u32 = 1u << 1u;
const WORLD_GI_RADIANCE_CACHE_MAX_PROBE_COUNT : u32 = 16u;

const WORLD_GI_SHADOW_PROBE_FLAG_VALID : u32 = 1u << 0u;
const WORLD_GI_SHADOW_PROBE_FLAG_ACTIVE_RAY : u32 = 1u << 1u;
const WORLD_GI_SHADOW_RESERVOIR_FLAG_VALID : u32 = 1u << 0u;
const WORLD_GI_SHADOW_PROBE_OCT_RESOLUTION : u32 = 8u;
const WORLD_GI_SHADOW_PROBE_OCT_TEXEL_COUNT : u32 = 64u;
const WORLD_GI_SHADOW_RAYS_PER_PROBE : u32 = 4u;
const WORLD_GI_SHADOW_RESERVOIR_SLOTS_PER_PROBE : u32 = WORLD_GI_SHADOW_RAYS_PER_PROBE;
const WORLD_GI_SHADOW_TEMPORAL_SAMPLE_COUNT : f32 = 64.0;

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
    // sphere radius / planar half extents, or sky ground color + exponent
    shapeParams : vec4<f32>,
    colorIntensity : vec4<f32>,
    flags : vec4<u32>,
};

struct WorldGIPlanarLightBasis {
    tangent : vec3<f32>,
    bitangent : vec3<f32>,
    normal : vec3<f32>,
};

fn worldgi_planar_light_basis(light : WorldGILight) -> WorldGIPlanarLightBasis {
    var normal = light.directionType.xyz;
    let normalLength = length(normal);
    if (normalLength > 1.0e-6) {
        normal = normal / normalLength;
    } else {
        normal = vec3<f32>(0.0, -1.0, 0.0);
    }
    var reference = vec3<f32>(0.0, 1.0, 0.0);
    if (abs(normal.y) > 0.999) {
        reference = vec3<f32>(1.0, 0.0, 0.0);
    }
    let tangent = normalize(cross(reference, normal));
    return WorldGIPlanarLightBasis(tangent, cross(normal, tangent), normal);
}

fn worldgi_planar_light_closest_point(light : WorldGILight, position : vec3<f32>) -> vec3<f32> {
    let basis = worldgi_planar_light_basis(light);
    let offset = position - light.positionRange.xyz;
    let u = clamp(dot(offset, basis.tangent), -max(light.shapeParams.y, 0.0), max(light.shapeParams.y, 0.0));
    let v = clamp(dot(offset, basis.bitangent), -max(light.shapeParams.z, 0.0), max(light.shapeParams.z, 0.0));
    return light.positionRange.xyz + basis.tangent * u + basis.bitangent * v;
}

struct WorldGILightGridCell {
    itemCount : vec4<u32>,
};

struct WorldGILightBVHNode {
    boundsMinNodeType : vec4<f32>,
    boundsMaxImportance : vec4<f32>,
    childOrLightCount : vec4<u32>,
};

struct WorldGILightBVHInfo {
    nodeCount : u32,
    rootNodeIndex : u32,
    lightCount : u32,
    infiniteLightCount : u32,
};

struct WorldGIShadowProbe {
    worldPositionDepth : vec4<f32>,
    normalConfidence : vec4<f32>,
    pixelLightFlags : vec4<u32>,
    rayDirectionMaxT : vec4<f32>,
    lightRadiancePdf : vec4<f32>,
    visibilityVarianceHistory : vec4<f32>,
};

struct WorldGIShadowLightReservoir {
    lightSampleFlags : vec4<u32>,
    weightTargetConfidence : vec4<f32>,
    radianceVisibility : vec4<f32>,
    worldPositionDepth : vec4<f32>,
    normalHistory : vec4<f32>,
    sampleDirectionDistance : vec4<f32>,
};

struct GIWorldRadianceCacheEntry {
    keyX : atomic<u32>,
    keyY : atomic<u32>,
    keyZ : atomic<u32>,
    flags : atomic<u32>,
    worldPositionCellSize : vec4<f32>,
    normalConfidence : vec4<f32>,
    baseColorRoughness : vec4<f32>,
    emissiveRadiance : vec4<f32>,
    directDiffuseRadiance : vec4<f32>,
    directSpecularRadiance : vec4<f32>,
    indirectDiffuseIrradianceSH0 : vec4<f32>,
    glossyRadiance : vec4<f32>,
    metadata : vec4<u32>,
};

struct GIWorldRadianceCacheActiveItem {
    entrySurfaceProducerFlags : vec4<u32>,
};

struct GIWorldRadianceCacheCounters {
    activeCount : atomic<u32>,
    touchedCount : atomic<u32>,
    reusedCount : atomic<u32>,
    overflowCount : atomic<u32>,
};
