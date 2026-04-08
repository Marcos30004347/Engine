struct HeatmapUniforms {
    width          : u32,
    height         : u32,
    blueThreshold  : u32,
    greenThreshold : u32,
    redThreshold   : u32,
};

@group(0) @binding(0) var<uniform> uniforms : HeatmapUniforms;
@group(0) @binding(1) var<storage, read> statistics : array<u32>;
@group(0) @binding(2) var outputTexture : texture_storage_2d<rgba16float, write>;

/// @brief Normalizes a value into the [0, 1] range within a given band [startValue, endValue].
/// @param value The raw statistic value to normalize.
/// @param startValue The lower bound of the band; values at or below this map to 0.
/// @param endValue The upper bound of the band; values at or above this map to 1.
/// @returns A normalized float in [0, 1] representing the value's position within the band.
fn normalizeBand(value: u32, startValue: u32, endValue: u32) -> f32 {
    if (endValue <= startValue) {
        return 1.0;
    }
    let clamped = clamp(f32(value), f32(startValue), f32(endValue));
    return (clamped - f32(startValue)) / f32(endValue - startValue);
}

/// @brief Maps a raw statistic counter value to a heatmap RGBA colour (black → blue → green → red).
/// @param value The per-pixel statistic count to colour.
/// @returns An RGBA colour representing the statistical intensity of the pixel.
fn heatmapColor(value: u32) -> vec4<f32> {
    if (value == 0u) {
        return vec4<f32>(0.0, 0.0, 0.0, 1.0);
    }

    if (value < uniforms.blueThreshold) {
        return vec4<f32>(0.0, 0.0, normalizeBand(value, 0u, uniforms.blueThreshold), 1.0);
    }

    if (value < uniforms.greenThreshold) {
        return vec4<f32>(0.0, normalizeBand(value, uniforms.blueThreshold, uniforms.greenThreshold), 0.0, 1.0);
    }

    return vec4<f32>(normalizeBand(value, uniforms.greenThreshold, uniforms.redThreshold), 0.0, 0.0, 1.0);
}

/// @brief Compute entry point that converts per-pixel frame statistics into a heatmap colour texture.
@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) gid : vec3<u32>) {
    if (gid.x >= uniforms.width || gid.y >= uniforms.height) {
        return;
    }

    let index = gid.y * uniforms.width + gid.x;
    if (index >= arrayLength(&statistics)) {
        return;
    }

    textureStore(outputTexture, vec2<i32>(gid.xy), heatmapColor(statistics[index]));
}
