@group(0) @binding(1) var texSampler   : sampler;
@group(0) @binding(2) var inputTexture : texture_2d<f32>;

/// @brief Maps a normalised value in [0, 1] to a blue-cyan-yellow-red heatmap colour.
/// @param t The normalised input value to colourise.
/// @returns An RGB colour representing the position of t along the heatmap gradient.
fn heatmap(t: f32) -> vec3<f32> {
    let x = clamp(t, 0.0, 1.0);
    if (x < 0.25) {
        return mix(vec3<f32>(0.02, 0.05, 0.12), vec3<f32>(0.11, 0.44, 0.79), x / 0.25);
    }
    if (x < 0.5) {
        return mix(vec3<f32>(0.11, 0.44, 0.79), vec3<f32>(0.16, 0.76, 0.51), (x - 0.25) / 0.25);
    }
    if (x < 0.75) {
        return mix(vec3<f32>(0.16, 0.76, 0.51), vec3<f32>(0.98, 0.84, 0.20), (x - 0.5) / 0.25);
    }
    return mix(vec3<f32>(0.98, 0.84, 0.20), vec3<f32>(0.92, 0.27, 0.18), (x - 0.75) / 0.25);
}

/// @brief Fragment entry point that visualises a shadow mask texture with boosted contrast using a heatmap.
@fragment
fn fs_main(@location(0) uv : vec2<f32>) -> @location(0) vec4<f32> {
    let v = textureSample(inputTexture, texSampler, uv).r;
    let av = abs(v);
    let hasData = av > 1e-7;
    if (!hasData) {
        return vec4<f32>(0.01, 0.01, 0.015, 1.0);
    }

    let boosted = clamp(1.0 - exp(-av * 256.0), 0.0, 1.0);
    let visible = max(0.18, max(boosted, clamp(v, 0.0, 1.0)));
    let color = heatmap(visible);
    return vec4<f32>(color, 1.0);
}
