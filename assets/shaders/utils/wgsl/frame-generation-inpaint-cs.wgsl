// Resolves interpolation holes from the finest valid level of the color
// pyramid. Partially valid pixels blend toward the inpainted estimate so edge
// coverage changes remain soft instead of popping between valid/invalid.

struct InpaintUniforms {
    outputExtent : vec4<u32>,
    flags : vec4<u32>, // x debug mode
};

@group(0) @binding(0) var<uniform> uniforms : InpaintUniforms;
@group(0) @binding(1) var rawTexture : texture_2d<f32>;
@group(0) @binding(2) var pyramid0Texture : texture_2d<f32>;
@group(0) @binding(3) var pyramid1Texture : texture_2d<f32>;
@group(0) @binding(4) var pyramid2Texture : texture_2d<f32>;
@group(0) @binding(5) var pyramid3Texture : texture_2d<f32>;
@group(0) @binding(6) var outputTexture : texture_storage_2d<rgba16float, write>;

fn clamp_pixel(pixel : vec2<i32>, extent : vec2<u32>) -> vec2<i32> {
    return clamp(pixel, vec2<i32>(0), vec2<i32>(extent) - vec2<i32>(1));
}

fn sample_nearest(inputTexture : texture_2d<f32>, uv : vec2<f32>) -> vec4<f32> {
    let extent = textureDimensions(inputTexture);
    return textureLoad(
        inputTexture,
        clamp_pixel(vec2<i32>(uv * vec2<f32>(extent)), extent), 0);
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) invocation : vec3<u32>) {
    let extent = uniforms.outputExtent.xy;
    if (any(invocation.xy >= extent)) {
        return;
    }
    let pixel = vec2<i32>(invocation.xy);
    let uv = (vec2<f32>(invocation.xy) + vec2<f32>(0.5)) / vec2<f32>(extent);
    let raw = textureLoad(rawTexture, pixel, 0);
    var replacement = sample_nearest(pyramid3Texture, uv);
    let level2 = sample_nearest(pyramid2Texture, uv);
    let level1 = sample_nearest(pyramid1Texture, uv);
    let level0 = sample_nearest(pyramid0Texture, uv);
    if (level2.a > 0.0) { replacement = level2; }
    if (level1.a > 0.0) { replacement = level1; }
    if (level0.a > 0.0) { replacement = level0; }
    let replacementColor = select(raw.rgb, replacement.rgb, replacement.a > 0.0);
    let result = mix(replacementColor, raw.rgb, clamp(raw.a, 0.0, 1.0));
    textureStore(outputTexture, pixel, vec4<f32>(result, 1.0));
}
