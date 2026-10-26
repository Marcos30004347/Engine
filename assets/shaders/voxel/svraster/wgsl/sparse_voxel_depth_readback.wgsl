#include "sparse_voxel_types.wgsl"

@group(0) @binding(0) var<uniform> uniforms : SparseVoxelRasterUniforms;
@group(0) @binding(1) var depthTexture : texture_depth_2d;
@group(0) @binding(2) var<storage, read_write> outputDepth : array<f32>;

@compute @workgroup_size(8, 8, 1)
fn readVoxelDepth(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let pixel = globalId.xy;
    if (any(pixel >= uniforms.viewportTiles.xy)) {
        return;
    }
    let pixelIndex = pixel.y * uniforms.viewportTiles.x + pixel.x;
    outputDepth[pixelIndex] = textureLoad(depthTexture, vec2<i32>(pixel), 0);
}
