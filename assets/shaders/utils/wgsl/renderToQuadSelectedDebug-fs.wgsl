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
    _padding0    : u32,
    _padding1    : u32,
    _padding2    : u32,
};

const TARGET_SLOT_BASE_COLOR         : u32 = 0u;
const TARGET_SLOT_CLUSTER_ID         : u32 = 1u;
const TARGET_SLOT_PREPASS_DEPTH      : u32 = 2u;
const TARGET_SLOT_SCENE_DEPTH        : u32 = 3u;
const TARGET_SLOT_HARDWARE_STATS     : u32 = 4u;
const TARGET_SLOT_VSM_PAGES          : u32 = 5u;
const TARGET_SLOT_VSM_TABLE          : u32 = 6u;
const TARGET_SLOT_VSM_SHADOW_LIGHTING: u32 = 7u;
const TARGET_SLOT_VSM_SCREEN_SPACE_SHADOW: u32 = 8u;

@group(0) @binding(0) var<uniform> u : QuadUniforms;
@group(0) @binding(1) var texSampler : sampler;
@group(0) @binding(2) var baseColorTexture : texture_2d<f32>;
@group(0) @binding(3) var clusterIdTexture : texture_2d<f32>;
@group(0) @binding(4) var prepassDepthTexture : texture_2d<f32>;
@group(0) @binding(5) var sceneDepthTexture : texture_depth_2d;
@group(0) @binding(6) var hardwareStatsTexture : texture_2d<f32>;
@group(0) @binding(7) var vsmPagesDebugTexture : texture_2d<f32>;
@group(0) @binding(8) var vsmTableDebugTexture : texture_2d<f32>;
@group(0) @binding(9) var vsmShadowLightingTexture : texture_2d<f32>;
@group(0) @binding(10) var vsmScreenSpaceShadowTexture : texture_2d<f32>;

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

/// @brief Fragment entry point that routes UV sampling to one of several debug visualisation modes based on the selected slot uniform.
@fragment
fn fs_main(@location(0) uv : vec2<f32>) -> @location(0) vec4<f32> {
    switch (u.selectedSlot) {
        case TARGET_SLOT_BASE_COLOR: {
            return textureSample(baseColorTexture, texSampler, uv);
        }
        case TARGET_SLOT_CLUSTER_ID: {
            return textureSample(clusterIdTexture, texSampler, uv);
        }
        case TARGET_SLOT_PREPASS_DEPTH: {
            return scalarDebug(textureSample(prepassDepthTexture, texSampler, uv).r);
        }
        case TARGET_SLOT_SCENE_DEPTH: {
            return depthDebug(textureSample(sceneDepthTexture, texSampler, uv));
        }
        case TARGET_SLOT_HARDWARE_STATS: {
            return textureSample(hardwareStatsTexture, texSampler, uv);
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
        default: {
            return vec4<f32>(1.0, 0.0, 1.0, 1.0);
        }
    }
}
