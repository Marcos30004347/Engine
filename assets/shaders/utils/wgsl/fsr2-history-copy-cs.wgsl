#include "fsr2-temporal-common.wgsl"

@group(0) @binding(0) var<uniform> uniforms : Fsr2Uniforms;
@group(0) @binding(6) var dilatedMotionTexture : texture_2d<f32>;
@group(0) @binding(12) var outputDilatedMotionTexture : texture_storage_2d<rgba16float, write>;

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) globalInvocationId : vec3<u32>) {
    let sourcePixel = vec2<i32>(globalInvocationId.xy);
    if (globalInvocationId.x < uniforms.sourceViewport.x && globalInvocationId.y < uniforms.sourceViewport.y) {
        textureStore(outputDilatedMotionTexture, sourcePixel, textureLoad(dilatedMotionTexture, sourcePixel, 0));
    }
}
