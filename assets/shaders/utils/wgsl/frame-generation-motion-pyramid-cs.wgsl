// Builds the game-motion inpainting pyramid. The first level gathers the
// raster motion field and rejects disoccluded or implausibly large vectors;
// later levels average only valid motion. Alpha carries accumulated validity.

struct MotionPyramidUniforms {
    extent : vec4<u32>, // xy source, zw destination
    controls : vec4<f32>, // x max pixels, yz output extent
    flags : vec4<u32>, // x source is an RGBA motion-pyramid level
};

@group(0) @binding(0) var<uniform> uniforms : MotionPyramidUniforms;
@group(0) @binding(1) var inputTexture : texture_2d<f32>;
@group(0) @binding(2) var disocclusionTexture : texture_2d<f32>;
@group(0) @binding(3) var outputTexture : texture_storage_2d<rgba16float, write>;

fn clamp_pixel(pixel : vec2<i32>, extent : vec2<u32>) -> vec2<i32> {
    return clamp(pixel, vec2<i32>(0), vec2<i32>(extent) - vec2<i32>(1));
}

fn sample_nearest(input : texture_2d<f32>, uv : vec2<f32>) -> vec4<f32> {
    let extent = textureDimensions(input);
    return textureLoad(
        input, clamp_pixel(vec2<i32>(uv * vec2<f32>(extent)), extent), 0);
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) invocation : vec3<u32>) {
    let destinationExtent = uniforms.extent.zw;
    if (any(invocation.xy >= destinationExtent)) {
        return;
    }

    var weightedMotion = vec2<f32>(0.0);
    var validity = 0.0;
    for (var y = 0; y < 2; y++) {
        for (var x = 0; x < 2; x++) {
            let uv = (vec2<f32>(invocation.xy * 2u + vec2<u32>(u32(x), u32(y))) +
                vec2<f32>(0.5)) / vec2<f32>(destinationExtent * 2u);
            let value = sample_nearest(inputTexture, uv);
            var weight = clamp(value.a, 0.0, 1.0);
            if (uniforms.flags.x == 0u) {
                let motion = value.xy;
                let outputExtent = max(uniforms.controls.yz, vec2<f32>(1.0));
                let plausible = all(abs(motion) < vec2<f32>(1.0e3)) &&
                    length(motion * outputExtent) <= max(uniforms.controls.x, 1.0);
                let disocclusion = clamp(
                    sample_nearest(disocclusionTexture, uv).x, 0.0, 1.0);
                weight = select(0.0, 1.0 - disocclusion, plausible);
            }
            weightedMotion += value.xy * weight;
            validity += weight;
        }
    }
    let motion = select(
        vec2<f32>(0.0), weightedMotion / max(validity, 1.0e-5),
        validity > 0.0);
    textureStore(
        outputTexture, vec2<i32>(invocation.xy),
        vec4<f32>(motion, 0.0, min(validity, 1.0)));
}
