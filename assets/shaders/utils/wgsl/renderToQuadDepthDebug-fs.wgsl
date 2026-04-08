// Depth debug fullscreen quad fragment shader
//
// Samples a depth texture and remaps reverse-Z values into a visible heat map
// so page-selection inputs can be inspected next to the VSM debug overlays.

@group(0) @binding(1) var texSampler   : sampler;
@group(0) @binding(2) var inputTexture : texture_depth_2d;

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

/// @brief Fragment entry point that samples a depth texture and remaps reverse-Z values to a heatmap for debugging.
@fragment
fn fs_main(@location(0) uv : vec2<f32>) -> @location(0) vec4<f32> {
    let depthValue = textureSample(inputTexture, texSampler, uv);
    let emphasized = clamp(1.0 - exp(-depthValue * 48.0), 0.0, 1.0);
    return vec4<f32>(heatmap(emphasized), 1.0);
}
