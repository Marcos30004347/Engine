// Downsamples interpolated color while treating alpha as validity. Invalid
// samples contribute no color, so progressively coarser levels approximate the
// nearest valid 2D neighborhood instead of smearing black holes.

struct PyramidUniforms {
    extent : vec4<u32>, // xy source, zw destination
};

@group(0) @binding(0) var<uniform> uniforms : PyramidUniforms;
@group(0) @binding(1) var inputTexture : texture_2d<f32>;
@group(0) @binding(2) var outputTexture : texture_storage_2d<rgba16float, write>;

fn clamp_pixel(pixel : vec2<i32>, extent : vec2<u32>) -> vec2<i32> {
    return clamp(pixel, vec2<i32>(0), vec2<i32>(extent) - vec2<i32>(1));
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) invocation : vec3<u32>) {
    let sourceExtent = uniforms.extent.xy;
    let destinationExtent = uniforms.extent.zw;
    if (any(invocation.xy >= destinationExtent)) {
        return;
    }
    let sourceBase = vec2<i32>(invocation.xy * 2u);
    var weightedColor = vec3<f32>(0.0);
    var validity = 0.0;
    for (var y = 0; y < 2; y++) {
        for (var x = 0; x < 2; x++) {
            let sampleValue = textureLoad(
                inputTexture,
                clamp_pixel(sourceBase + vec2<i32>(x, y), sourceExtent), 0);
            let weight = clamp(sampleValue.a, 0.0, 1.0);
            weightedColor += sampleValue.rgb * weight;
            validity += weight;
        }
    }
    let color = select(
        vec3<f32>(0.0), weightedColor / max(validity, 1.0e-5),
        validity > 0.0);
    textureStore(
        outputTexture, vec2<i32>(invocation.xy),
        vec4<f32>(color, min(validity, 1.0)));
}
