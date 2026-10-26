// Builds one level of the ping-ponged luminance pyramid used by optical flow.
// Level zero converts HDR color to log luminance; later levels average 2x2
// luminance footprints. Log luminance keeps a bright light from dominating the
// SAD metric used by the flow search.

struct PyramidUniforms {
    extent : vec4<u32>, // xy source, zw destination
    flags : vec4<u32>,  // x: source is already luminance
};

@group(0) @binding(0) var<uniform> uniforms : PyramidUniforms;
@group(0) @binding(1) var inputTexture : texture_2d<f32>;
@group(0) @binding(2) var outputTexture : texture_storage_2d<r32float, write>;

fn clamp_pixel(pixel : vec2<i32>, extent : vec2<u32>) -> vec2<i32> {
    return clamp(pixel, vec2<i32>(0), vec2<i32>(extent) - vec2<i32>(1));
}

fn input_luma(pixel : vec2<i32>, extent : vec2<u32>) -> f32 {
    let value = textureLoad(inputTexture, clamp_pixel(pixel, extent), 0);
    if (uniforms.flags.x != 0u) {
        return value.x;
    }
    return log2(1.0 + max(dot(value.rgb, vec3<f32>(0.2126, 0.7152, 0.0722)), 0.0));
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) invocation : vec3<u32>) {
    let sourceExtent = uniforms.extent.xy;
    let destinationExtent = uniforms.extent.zw;
    if (any(invocation.xy >= destinationExtent)) {
        return;
    }
    let sourceBase = vec2<i32>(invocation.xy * 2u);
    let value = 0.25 * (
        input_luma(sourceBase, sourceExtent) +
        input_luma(sourceBase + vec2<i32>(1, 0), sourceExtent) +
        input_luma(sourceBase + vec2<i32>(0, 1), sourceExtent) +
        input_luma(sourceBase + vec2<i32>(1, 1), sourceExtent));
    textureStore(outputTexture, vec2<i32>(invocation.xy), vec4<f32>(value));
}
