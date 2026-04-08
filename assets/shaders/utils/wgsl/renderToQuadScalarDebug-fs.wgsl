@group(0) @binding(1) var texSampler   : sampler;
@group(0) @binding(2) var inputTexture : texture_2d<f32>;

/// @brief Fragment entry point that visualises a scalar texture channel as a greyscale image with gamma and log tone-mapping for debugging.
@fragment
fn fs_main(@location(0) uv : vec2<f32>) -> @location(0) vec4<f32> {
    let v = textureSample(inputTexture, texSampler, uv).r;
    let z = clamp(abs(v), 0.0, 1.0);
    // Expand both the very small reverse-Z range and the high-end range so
    // HZB mips remain visible even when most values are close to zero.
    let lifted = pow(z, 0.2);
    let logged = clamp(log2(1.0 + z * 65535.0) / 16.0, 0.0, 1.0);
    let gray = select(0.0, max(lifted, logged), z > 1e-8);
    return vec4<f32>(gray, gray, gray, 1.0);
}
