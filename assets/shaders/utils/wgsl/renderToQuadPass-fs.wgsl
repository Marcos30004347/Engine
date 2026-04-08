// quad-fs.wgsl
//
// Samples the input texture at the UV coordinates produced by the vertex
// shader and writes the result straight to the colour attachment.
// The mip level and array layer are baked into the TextureView on the C++
// side (baseMipLevel / baseArrayLayer), so the sampler always resolves to
// the correct mip/layer without any shader-side logic.

@group(0) @binding(1) var texSampler  : sampler;
@group(0) @binding(2) var inputTexture : texture_2d<f32>;

/// @brief Fragment entry point that samples the input texture at the interpolated UV and writes it to the colour attachment.
@fragment
fn fs_main(@location(0) uv : vec2<f32>) -> @location(0) vec4<f32> {
    return textureSample(inputTexture, texSampler, uv);
}
