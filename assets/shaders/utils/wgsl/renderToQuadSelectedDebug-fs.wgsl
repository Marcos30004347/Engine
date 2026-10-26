struct QuadUniforms {
    ndcMinX : f32,
    ndcMinY : f32,
    ndcMaxX : f32,
    ndcMaxY : f32,
    uvMinX  : f32,
    uvMinY  : f32,
    uvMaxX  : f32,
    uvMaxY  : f32,
    selectedSlot : u32,
    _padding0    : vec3<u32>,
};

const TARGET_SLOT_SHADED_COLOR       : u32 = 0u;
const TARGET_SLOT_DEBUG_COLOR        : u32 = 1u;
const TARGET_SLOT_PREPASS_DEPTH      : u32 = 2u;
const TARGET_SLOT_SCENE_DEPTH        : u32 = 3u;
const TARGET_SLOT_OVERDRAW           : u32 = 4u;
const TARGET_SLOT_MATERIAL_ID        : u32 = 5u;
const TARGET_SLOT_MOTION_VECTORS     : u32 = 6u;
const TARGET_SLOT_VSM_PAGES          : u32 = 7u;
const TARGET_SLOT_VSM_TABLE          : u32 = 8u;
const TARGET_SLOT_VSM_SHADOW_LIGHTING: u32 = 9u;
const TARGET_SLOT_VSM_SCREEN_SPACE_SHADOW: u32 = 10u;
const TARGET_SLOT_TRIANGLE_ID        : u32 = 11u;
const TARGET_SLOT_CLUSTER_ID         : u32 = 12u;
const TARGET_SLOT_SDF_TRACE          : u32 = 13u;
const TARGET_SLOT_SDF_ATLAS          : u32 = 14u;
const TARGET_SLOT_SDF_FALLBACK_SCENE : u32 = 15u;
const TARGET_SLOT_RAY_TRACE_SCENE_PRIMARY_RAYS : u32 = 16u;
const TARGET_SLOT_BASE_COLOR         : u32 = 18u;
const TARGET_SLOT_NORMAL             : u32 = 19u;
const TARGET_SLOT_GEOMETRY_NORMAL    : u32 = 20u;
const TARGET_SLOT_DISOCCLUSION_MASK  : u32 = 21u;
const TARGET_SLOT_SCREEN_PROBES      : u32 = 22u;
const TARGET_SLOT_SPATIAL_CACHE      : u32 = 23u;
const TARGET_SLOT_SCREEN_PROBE_INDIRECT_DIFFUSE : u32 = 24u;
const TARGET_SLOT_SCREEN_PROBE_BENT_NORMAL : u32 = 25u;
const TARGET_SLOT_SCREEN_PROBE_AMBIENT_OCCLUSION : u32 = 26u;
const TARGET_SLOT_SCREEN_PROBE_AO_DEBUG_DISTANCE : u32 = 27u;
const TARGET_SLOT_SCREEN_PROBE_AO_DEBUG_CASCADE : u32 = 28u;
const TARGET_SLOT_SCREEN_PROBE_AO_DEBUG_FLAGS : u32 = 29u;
const TARGET_SLOT_SCREEN_PROBE_AO_DEBUG_STEPS : u32 = 30u;
const TARGET_SLOT_SPATIAL_RADIANCE_RESERVOIR_CACHE : u32 = 39u;
const TARGET_SLOT_SURFEL_HIERARCHY : u32 = 40u;
const TARGET_SLOT_SURFEL_DIRECT_DRAW : u32 = 41u;
const TARGET_SLOT_SURFEL_HEATMAP : u32 = 42u;
const TARGET_SLOT_SCREEN_SPACE_RAY_TRACE_STEPS : u32 = 43u;
const TARGET_SLOT_VSM_ATLAS          : u32 = 44u;
const TARGET_SLOT_SDF_SURFACE_CACHE_ALBEDO_ATLAS : u32 = 49u;
const TARGET_SLOT_SDF_SURFACE_CACHE_NORMAL_ATLAS : u32 = 50u;
const TARGET_SLOT_VSM_SCREEN_SPACE_SOFT_SHADOW: u32 = 56u;

@group(0) @binding(0) var<uniform> u : QuadUniforms;
@group(0) @binding(1) var texSampler : sampler;
@group(0) @binding(2) var shadedColorTexture : texture_2d<f32>;
@group(0) @binding(3) var debugColorTexture : texture_2d<f32>;
@group(0) @binding(4) var baseColorTexture : texture_2d<f32>;
@group(0) @binding(5) var geometryNormalTexture : texture_2d<f32>;
@group(0) @binding(6) var normalTexture : texture_2d<f32>;
@group(0) @binding(7) var prepassDepthTexture : texture_2d<f32>;
@group(0) @binding(8) var sceneDepthTexture : texture_depth_2d;
@group(0) @binding(9) var overdrawTexture : texture_2d<f32>;
@group(0) @binding(10) var materialIdTexture : texture_2d<u32>;
@group(0) @binding(11) var motionVectorTexture : texture_2d<f32>;
@group(0) @binding(12) var vsmPagesDebugTexture : texture_2d<f32>;
@group(0) @binding(13) var vsmTableDebugTexture : texture_2d<f32>;
@group(0) @binding(14) var vsmShadowLightingTexture : texture_2d<f32>;
@group(0) @binding(15) var vsmScreenSpaceShadowTexture : texture_2d<f32>;
@group(0) @binding(16) var packedGeometryIdsLoTexture : texture_2d<u32>;
@group(0) @binding(17) var packedGeometryIdsHiTexture : texture_2d<u32>;
@group(0) @binding(18) var signedDistanceFieldsTraceTexture : texture_2d<f32>;
@group(0) @binding(19) var signedDistanceFieldsAtlasTexture : texture_2d<f32>;
@group(0) @binding(20) var signedDistanceFieldsFallbackSceneTexture : texture_2d<f32>;
@group(0) @binding(21) var rayTraceScenePrimaryRaysTexture : texture_2d<f32>;
@group(0) @binding(22) var disocclusionMaskTexture : texture_2d<f32>;
@group(0) @binding(23) var screenProbeDebugTexture : texture_2d<f32>;
@group(0) @binding(24) var spatialRadianceCacheTexture : texture_2d<f32>;
@group(0) @binding(25) var screenProbeIndirectDiffuseTexture : texture_2d<f32>;
@group(0) @binding(26) var screenProbeBentNormalTexture : texture_2d<f32>;
@group(0) @binding(27) var screenProbeAmbientOcclusionTexture : texture_2d<f32>;
@group(0) @binding(28) var screenProbeAODebugTexture : texture_2d<f32>;
@group(0) @binding(37) var spatialRadianceReservoirCacheTexture : texture_2d<f32>;
@group(0) @binding(38) var surfelHierarchyTexture : texture_2d<f32>;
@group(0) @binding(39) var surfelDirectDrawTexture : texture_2d<f32>;
@group(0) @binding(40) var surfelHeatmapTexture : texture_2d<f32>;
@group(0) @binding(41) var screenSpaceRayTraceStepsTexture : texture_2d<u32>;
@group(0) @binding(42) var vsmAtlasDebugTexture : texture_2d<f32>;
@group(0) @binding(46) var signedDistanceFieldsSurfaceCacheAlbedoAtlasTexture : texture_2d<f32>;
@group(0) @binding(47) var signedDistanceFieldsSurfaceCacheNormalAtlasTexture : texture_2d<f32>;
@group(0) @binding(51) var vsmScreenSpaceSoftShadowTexture : texture_2d<f32>;

// The packed identity: full instance index in the low word, the visibility
// payload (globalCluster << 7 | triangle) in the high word - the encoding
// both rasterizers emit (see packVisibilityPayload in virtualgeometrydata.wgsl).
struct GeometryIds {
    instanceIndex : u32,
    globalCluster : u32,
    triangleId : u32,
};

/// @brief Converts a scalar float value into a greyscale debug colour using gamma and log tone-mapping.
/// @param v The scalar value to visualise.
/// @returns An RGBA greyscale colour suitable for debug inspection.
fn scalarDebug(v: f32) -> vec4<f32> {
    let z = clamp(abs(v), 0.0, 1.0);
    let lifted = pow(z, 0.2);
    let logged = clamp(log2(1.0 + z * 65535.0) / 16.0, 0.0, 1.0);
    let gray = select(0.0, max(lifted, logged), z > 1e-8);
    return vec4<f32>(gray, gray, gray, 1.0);
}

/// @brief Maps a normalised value in [0, 1] to a blue-cyan-yellow-red heatmap colour.
/// @param t The normalised input value to colourise.
/// @returns An RGB colour representing the position of t along the heatmap gradient.
fn heatmap(t: f32) -> vec3<f32> {
    let x = clamp(t, 0.0, 1.0);
    if (x < 0.25) {
        return mix(vec3<f32>(0.02, 0.03, 0.08), vec3<f32>(0.08, 0.30, 0.75), x / 0.25);
    }
    if (x < 0.5) {
        return mix(vec3<f32>(0.08, 0.30, 0.75), vec3<f32>(0.10, 0.72, 0.50), (x - 0.25) / 0.25);
    }
    if (x < 0.75) {
        return mix(vec3<f32>(0.10, 0.72, 0.50), vec3<f32>(0.98, 0.80, 0.18), (x - 0.5) / 0.25);
    }
    return mix(vec3<f32>(0.98, 0.80, 0.18), vec3<f32>(0.92, 0.26, 0.12), (x - 0.75) / 0.25);
}

/// @brief Visualises a shadow mask value with boosted contrast so near-zero values remain visible.
/// @param v The raw shadow visibility value to visualise.
/// @returns An RGBA heatmap colour encoding the shadow intensity.
fn shadowDebug(v: f32) -> vec4<f32> {
    let av = abs(v);
    if (av <= 1e-7) {
        return vec4<f32>(0.01, 0.01, 0.015, 1.0);
    }

    let boosted = clamp(1.0 - exp(-av * 256.0), 0.0, 1.0);
    let visible = max(0.18, max(boosted, clamp(v, 0.0, 1.0)));
    return vec4<f32>(heatmap(visible), 1.0);
}

/// @brief Converts a linear depth value to a heatmap colour for depth buffer debugging.
/// @param depthValue The depth value (typically in [0, 1]) to visualise.
/// @returns An RGBA heatmap colour encoding the depth intensity.
fn depthDebug(depthValue: f32) -> vec4<f32> {
    let emphasized = clamp(1.0 - exp(-depthValue * 48.0), 0.0, 1.0);
    return vec4<f32>(heatmap(emphasized), 1.0);
}

fn hashDebugColor(value: u32) -> vec3<f32> {
    let x = f32(value);
    let r = fract(sin(x * 12.9898 + 0.1234) * 43758.5453);
    let g = fract(sin(x * 78.2330 + 1.2345) * 24634.6345);
    let b = fract(sin(x * 39.4250 + 2.3456) * 56445.2344);
    return 0.2 + 0.8 * vec3<f32>(r, g, b);
}

fn clampPixelCoords(uv: vec2<f32>) -> vec2<i32> {
    let dims = textureDimensions(materialIdTexture);
    return vec2<i32>(
        min(i32(uv.x * f32(dims.x)), i32(dims.x) - 1),
        min(i32(uv.y * f32(dims.y)), i32(dims.y) - 1)
    );
}

fn hasGeometry(pixel: vec2<i32>) -> bool {
    return textureLoad(sceneDepthTexture, pixel, 0) > 0.0;
}

fn unpackGeometryIds(pixel: vec2<i32>) -> GeometryIds {
    let lo = textureLoad(packedGeometryIdsLoTexture, pixel, 0).x;
    let hi = textureLoad(packedGeometryIdsHiTexture, pixel, 0).x;

    var ids : GeometryIds;
    ids.instanceIndex = lo;
    ids.globalCluster = hi >> 7u;
    ids.triangleId = hi & 0x7Fu;
    return ids;
}

fn materialIdDebug(uv: vec2<f32>) -> vec4<f32> {
    let pixel = clampPixelCoords(uv);
    if (!hasGeometry(pixel)) {
        return vec4<f32>(0.0, 0.0, 0.0, 1.0);
    }
    let materialId = textureLoad(materialIdTexture, pixel, 0).x;
    return vec4<f32>(hashDebugColor(materialId), 1.0);
}

fn triangleIdDebug(uv: vec2<f32>) -> vec4<f32> {
    let pixel = clampPixelCoords(uv);
    if (!hasGeometry(pixel)) {
        return vec4<f32>(0.0, 0.0, 0.0, 1.0);
    }

    let ids = unpackGeometryIds(pixel);
    return vec4<f32>(hashDebugColor(ids.triangleId), 1.0);
}

fn clusterIdDebug(uv: vec2<f32>) -> vec4<f32> {
    let pixel = clampPixelCoords(uv);
    if (!hasGeometry(pixel)) {
        return vec4<f32>(0.0, 0.0, 0.0, 1.0);
    }

    let ids = unpackGeometryIds(pixel);
    return vec4<f32>(hashDebugColor(ids.globalCluster), 1.0);
}

fn motionVectorDebug(uv: vec2<f32>) -> vec4<f32> {
    let motion = textureSample(motionVectorTexture, texSampler, uv).xy;
    let magnitude = length(motion);
    if (magnitude <= 1e-6) {
        return vec4<f32>(0.0, 0.0, 0.0, 1.0);
    }

    let direction = motion / magnitude;
    let visibleMagnitude = clamp(magnitude * 64.0, 0.0, 1.0);
    return vec4<f32>(
        0.5 + 0.5 * direction.x,
        0.5 - 0.5 * direction.y,
        visibleMagnitude,
        1.0
    );
}

fn normalDebug(uv: vec2<f32>) -> vec4<f32> {
    let normal = textureSample(normalTexture, texSampler, uv).xyz;
    let len = length(normal);
    if (len <= 1e-6) {
        return vec4<f32>(0.0, 0.0, 0.0, 1.0);
    }
    let n = normal / len;
    return vec4<f32>(n * 0.5 + 0.5, 1.0);
}

fn geometryNormalDebug(uv: vec2<f32>) -> vec4<f32> {
    let normal = textureSample(geometryNormalTexture, texSampler, uv).xyz;
    let len = length(normal);
    if (len <= 1e-6) {
        return vec4<f32>(0.0, 0.0, 0.0, 1.0);
    }
    let n = normal / len;
    return vec4<f32>(n * 0.5 + 0.5, 1.0);
}

fn indirectDiffuseDebug(uv: vec2<f32>) -> vec4<f32> {
    let color = max(textureSample(screenProbeIndirectDiffuseTexture, texSampler, uv).rgb, vec3<f32>(0.0));
    let tonemapped = color / (vec3<f32>(1.0) + color);
    let gammaCorrected = pow(tonemapped, vec3<f32>(1.0 / 2.2));
    return vec4<f32>(gammaCorrected, 1.0);
}

fn ambientOcclusionDebug(uv: vec2<f32>) -> vec4<f32> {
    let ao = clamp(textureSample(screenProbeAmbientOcclusionTexture, texSampler, uv).r, 0.0, 1.0);
    return vec4<f32>(vec3<f32>(ao), 1.0);
}

fn tonemappedTextureDebug(textureValue: texture_2d<f32>, uv: vec2<f32>) -> vec4<f32> {
    let color = max(textureSample(textureValue, texSampler, uv).rgb, vec3<f32>(0.0));
    let tonemapped = color / (vec3<f32>(1.0) + color);
    let gammaCorrected = pow(tonemapped, vec3<f32>(1.0 / 2.2));
    return vec4<f32>(gammaCorrected, 1.0);
}

fn normalTextureDebug(textureValue: texture_2d<f32>, uv: vec2<f32>) -> vec4<f32> {
    let normal = textureSample(textureValue, texSampler, uv).xyz;
    let len = length(normal);
    if (len <= 1e-6) {
        return vec4<f32>(0.0, 0.0, 0.0, 1.0);
    }
    let n = normal / len;
    return vec4<f32>(n * 0.5 + 0.5, 1.0);
}

fn worldPositionTextureDebug(textureValue: texture_2d<f32>, uv: vec2<f32>) -> vec4<f32> {
    let p = textureSample(textureValue, texSampler, uv).xyz;
    let compressed = p / (vec3<f32>(1.0) + abs(p));
    return vec4<f32>(compressed * 0.5 + vec3<f32>(0.5), 1.0);
}

fn screenProbeAODebugDistance(uv: vec2<f32>) -> vec4<f32> {
    let debugSample = textureSample(screenProbeAODebugTexture, texSampler, uv);
    if (debugSample.a <= 1.0e-5) {
        return vec4<f32>(0.0, 0.0, 0.0, 1.0);
    }
    return vec4<f32>(heatmap(1.0 - clamp(debugSample.r, 0.0, 1.0)), 1.0);
}

fn screenProbeAODebugCascade(uv: vec2<f32>) -> vec4<f32> {
    let debugSample = textureSample(screenProbeAODebugTexture, texSampler, uv);
    if (debugSample.a <= 1.0e-5) {
        return vec4<f32>(0.0, 0.0, 0.0, 1.0);
    }
    let cascadeIndex = u32(round(clamp(debugSample.g, 0.0, 1.0) * 7.0));
    return vec4<f32>(hashDebugColor(cascadeIndex + 1u), 1.0);
}

fn screenProbeAODebugFlags(uv: vec2<f32>) -> vec4<f32> {
    let debugSample = textureSample(screenProbeAODebugTexture, texSampler, uv);
    if (debugSample.a <= 1.0e-5) {
        return vec4<f32>(0.0, 0.0, 0.0, 1.0);
    }

    let reason = u32(round(clamp(debugSample.b, 0.0, 1.0) * 4.0));
    switch (reason) {
        case 1u: {
            return vec4<f32>(0.15, 0.82, 0.28, 1.0);
        }
        case 2u: {
            return vec4<f32>(0.95, 0.74, 0.12, 1.0);
        }
        case 3u: {
            return vec4<f32>(0.92, 0.20, 0.12, 1.0);
        }
        case 4u: {
            return vec4<f32>(0.12, 0.58, 0.95, 1.0);
        }
        default: {
            return vec4<f32>(0.10, 0.10, 0.10, 1.0);
        }
    }
}

fn screenProbeAODebugSteps(uv: vec2<f32>) -> vec4<f32> {
    let debugSample = textureSample(screenProbeAODebugTexture, texSampler, uv);
    if (debugSample.a <= 1.0e-5) {
        return vec4<f32>(0.0, 0.0, 0.0, 1.0);
    }
    return vec4<f32>(heatmap(clamp(debugSample.a, 0.0, 1.0)), 1.0);
}

fn screenSpaceRayTraceStepsDebug(uv: vec2<f32>) -> vec4<f32> {
    let dims = textureDimensions(screenSpaceRayTraceStepsTexture);
    let pixel = clamp(
        vec2<i32>(floor(uv * vec2<f32>(dims))),
        vec2<i32>(0, 0),
        vec2<i32>(dims) - vec2<i32>(1, 1));
    let steps = textureLoad(screenSpaceRayTraceStepsTexture, pixel, 0).r;
    if (steps == 0u) {
        return vec4<f32>(0.0, 0.0, 0.0, 1.0);
    }
    return vec4<f32>(heatmap(clamp(f32(steps) / 128.0, 0.0, 1.0)), 1.0);
}

/// @brief Fragment entry point that routes UV sampling to one of several debug visualisation modes based on the selected slot uniform.
@fragment
fn fs_main(@location(0) uv : vec2<f32>) -> @location(0) vec4<f32> {
    switch (u.selectedSlot) {
        case TARGET_SLOT_SHADED_COLOR: {
            return textureSample(shadedColorTexture, texSampler, uv);
        }
        case TARGET_SLOT_DEBUG_COLOR: {
            return textureSample(debugColorTexture, texSampler, uv);
        }
        case TARGET_SLOT_PREPASS_DEPTH: {
            return scalarDebug(textureSample(prepassDepthTexture, texSampler, uv).r);
        }
        case TARGET_SLOT_SCENE_DEPTH: {
            return depthDebug(textureSample(sceneDepthTexture, texSampler, uv));
        }
        case TARGET_SLOT_OVERDRAW: {
            return textureSample(overdrawTexture, texSampler, uv);
        }
        case TARGET_SLOT_MATERIAL_ID: {
            return materialIdDebug(uv);
        }
        case TARGET_SLOT_MOTION_VECTORS: {
            return motionVectorDebug(uv);
        }
        case TARGET_SLOT_VSM_PAGES: {
            return textureSample(vsmPagesDebugTexture, texSampler, uv);
        }
        case TARGET_SLOT_VSM_TABLE: {
            return textureSample(vsmTableDebugTexture, texSampler, uv);
        }
        case TARGET_SLOT_VSM_SHADOW_LIGHTING: {
            return textureSample(vsmShadowLightingTexture, texSampler, uv);
        }
        case TARGET_SLOT_VSM_SCREEN_SPACE_SHADOW: {
            return shadowDebug(textureSample(vsmScreenSpaceShadowTexture, texSampler, uv).r);
        }
        case TARGET_SLOT_VSM_SCREEN_SPACE_SOFT_SHADOW: {
            return textureSample(vsmScreenSpaceSoftShadowTexture, texSampler, uv);
        }
        case TARGET_SLOT_SDF_TRACE: {
            return textureSample(signedDistanceFieldsTraceTexture, texSampler, uv);
        }
        case TARGET_SLOT_SDF_ATLAS: {
            return textureSample(signedDistanceFieldsAtlasTexture, texSampler, uv);
        }
        case TARGET_SLOT_SDF_SURFACE_CACHE_ALBEDO_ATLAS: {
            return textureSample(signedDistanceFieldsSurfaceCacheAlbedoAtlasTexture, texSampler, uv);
        }
        case TARGET_SLOT_SDF_SURFACE_CACHE_NORMAL_ATLAS: {
            return textureSample(signedDistanceFieldsSurfaceCacheNormalAtlasTexture, texSampler, uv);
        }
        case TARGET_SLOT_SDF_FALLBACK_SCENE: {
            return textureSample(signedDistanceFieldsFallbackSceneTexture, texSampler, uv);
        }
        case TARGET_SLOT_RAY_TRACE_SCENE_PRIMARY_RAYS: {
            return textureSample(rayTraceScenePrimaryRaysTexture, texSampler, uv);
        }
        case TARGET_SLOT_TRIANGLE_ID: {
            return triangleIdDebug(uv);
        }
        case TARGET_SLOT_CLUSTER_ID: {
            return clusterIdDebug(uv);
        }
        case TARGET_SLOT_BASE_COLOR: {
            return textureSample(baseColorTexture, texSampler, uv);
        }
        case TARGET_SLOT_NORMAL: {
            return normalDebug(uv);
        }
        case TARGET_SLOT_GEOMETRY_NORMAL: {
            return geometryNormalDebug(uv);
        }
        case TARGET_SLOT_DISOCCLUSION_MASK: {
            return shadowDebug(textureSample(disocclusionMaskTexture, texSampler, uv).r);
        }
        case TARGET_SLOT_SCREEN_PROBES: {
            return textureSample(screenProbeDebugTexture, texSampler, uv);
        }
        case TARGET_SLOT_SPATIAL_CACHE: {
            return textureSample(spatialRadianceCacheTexture, texSampler, uv);
        }
        case TARGET_SLOT_SCREEN_PROBE_INDIRECT_DIFFUSE: {
            return indirectDiffuseDebug(uv);
        }
        case TARGET_SLOT_SCREEN_PROBE_BENT_NORMAL: {
            let normal = textureSample(screenProbeBentNormalTexture, texSampler, uv).xyz;
            let len = length(normal);
            if (len <= 1e-6) {
                return vec4<f32>(0.0, 0.0, 0.0, 1.0);
            }
            let n = normal / len;
            return vec4<f32>(n * 0.5 + 0.5, 1.0);
        }
        case TARGET_SLOT_SCREEN_PROBE_AMBIENT_OCCLUSION: {
            return ambientOcclusionDebug(uv);
        }
        case TARGET_SLOT_SCREEN_PROBE_AO_DEBUG_DISTANCE: {
            return screenProbeAODebugDistance(uv);
        }
        case TARGET_SLOT_SCREEN_PROBE_AO_DEBUG_CASCADE: {
            return screenProbeAODebugCascade(uv);
        }
        case TARGET_SLOT_SCREEN_PROBE_AO_DEBUG_FLAGS: {
            return screenProbeAODebugFlags(uv);
        }
        case TARGET_SLOT_SCREEN_PROBE_AO_DEBUG_STEPS: {
            return screenProbeAODebugSteps(uv);
        }
        case TARGET_SLOT_SPATIAL_RADIANCE_RESERVOIR_CACHE: {
            return tonemappedTextureDebug(spatialRadianceReservoirCacheTexture, uv);
        }
        case TARGET_SLOT_SURFEL_HIERARCHY: {
            return textureSample(surfelHierarchyTexture, texSampler, uv);
        }
        case TARGET_SLOT_SURFEL_DIRECT_DRAW: {
            return textureSample(surfelDirectDrawTexture, texSampler, uv);
        }
        case TARGET_SLOT_SURFEL_HEATMAP: {
            return textureSample(surfelHeatmapTexture, texSampler, uv);
        }
        case TARGET_SLOT_SCREEN_SPACE_RAY_TRACE_STEPS: {
            return screenSpaceRayTraceStepsDebug(uv);
        }
        case TARGET_SLOT_VSM_ATLAS: {
            return textureSample(vsmAtlasDebugTexture, texSampler, uv);
        }
        default: {
            return vec4<f32>(1.0, 0.0, 1.0, 1.0);
        }
    }
}
