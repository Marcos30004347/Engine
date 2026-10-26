struct SparseVoxelDepthPresentUniforms {
    width : u32,
    height : u32,
    reverseZ : u32,
    _reserved : u32,
}

@group(0) @binding(0) var<uniform> uniforms : SparseVoxelDepthPresentUniforms;
@group(0) @binding(1) var depthTexture : texture_depth_2d;
@group(0) @binding(2) var outputTexture : texture_storage_2d<rgba16float, write>;

@compute @workgroup_size(8, 8, 1)
fn presentVoxelDepth(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let pixel = globalId.xy;
    if (pixel.x >= uniforms.width || pixel.y >= uniforms.height) {
        return;
    }
    let deviceDepth = textureLoad(depthTexture, vec2<i32>(pixel), 0);
    let visibleDepth = select(1.0 - deviceDepth, deviceDepth, uniforms.reverseZ != 0u);
    textureStore(outputTexture, pixel, vec4<f32>(vec3<f32>(visibleDepth), 1.0));
}
