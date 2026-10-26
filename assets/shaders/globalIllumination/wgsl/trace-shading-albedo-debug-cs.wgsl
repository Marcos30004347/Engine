#include "globalIllumination/wgsl/trace-shading-types.wgsl"

struct TraceShadingAlbedoDebugUniforms {
    controls : vec4<u32>,
};

@group(0) @binding(0) var<uniform> uniforms : TraceShadingAlbedoDebugUniforms;
@group(0) @binding(1) var<storage, read> traceShading : array<TraceShadingParameters>;
@group(0) @binding(2) var outputTexture : texture_storage_2d<rgba16float, write>;

@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) gid : vec3<u32>) {
    let width = uniforms.controls.y;
    let height = uniforms.controls.z;
    if (gid.x >= width || gid.y >= height) {
        return;
    }

    let pixel = vec2<i32>(gid.xy);
    let rayId = gid.y * width + gid.x;
    if (uniforms.controls.x == 0u || rayId >= uniforms.controls.w || rayId >= arrayLength(&traceShading)) {
        textureStore(outputTexture, pixel, vec4<f32>(0.0, 0.0, 0.0, 1.0));
        return;
    }

    let shading = traceShading[rayId];
    if ((shading.ids.w & TRACE_SHADING_FLAG_VALID) == 0u) {
        textureStore(outputTexture, pixel, vec4<f32>(0.0, 0.0, 0.0, 1.0));
        return;
    }

    textureStore(outputTexture, pixel, vec4<f32>(clamp(shading.albedo.rgb, vec3<f32>(0.0), vec3<f32>(8.0)), 1.0));
}
