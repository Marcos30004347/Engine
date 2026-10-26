#include "sparse_voxel_types.wgsl"

@group(0) @binding(0) var<uniform> uniforms : SparseVoxelRasterUniforms;
@group(0) @binding(1) var instanceIdTexture : texture_2d<u32>;
@group(0) @binding(2) var voxelIdTexture : texture_2d<u32>;
@group(0) @binding(3) var materialIdTexture : texture_2d<u32>;
@group(0) @binding(4) var materialUvTexture : texture_2d<f32>;
@group(0) @binding(5) var baseColorTexture : texture_2d<f32>;
@group(0) @binding(6) var materialPropertiesTexture : texture_2d<f32>;
@group(0) @binding(7) var emissiveTexture : texture_2d<f32>;
@group(0) @binding(8) var geometryNormalTexture : texture_2d<f32>;
@group(0) @binding(9) var normalTexture : texture_2d<f32>;
@group(0) @binding(10) var motionVectorTexture : texture_2d<f32>;
@group(0) @binding(11) var depthTexture : texture_depth_2d;
@group(0) @binding(12) var<storage, read_write> outputPixels : array<SparseVoxelGBufferPixel>;

@compute @workgroup_size(8, 8, 1)
fn readVoxelGBuffer(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let pixel = globalId.xy;
    if (any(pixel >= uniforms.viewportTiles.xy)) {
        return;
    }
    let pixelI = vec2<i32>(pixel);
    let pixelIndex = pixel.y * uniforms.viewportTiles.x + pixel.x;
    let materialUv = textureLoad(materialUvTexture, pixelI, 0).xy;
    let motion = textureLoad(motionVectorTexture, pixelI, 0).xy;
    outputPixels[pixelIndex].ids = vec4<u32>(
        textureLoad(instanceIdTexture, pixelI, 0).x,
        textureLoad(voxelIdTexture, pixelI, 0).x,
        textureLoad(materialIdTexture, pixelI, 0).x,
        0u);
    outputPixels[pixelIndex].materialUv = vec4<f32>(materialUv, 0.0, 0.0);
    outputPixels[pixelIndex].baseColor = textureLoad(baseColorTexture, pixelI, 0);
    outputPixels[pixelIndex].materialProperties = textureLoad(materialPropertiesTexture, pixelI, 0);
    outputPixels[pixelIndex].emissive = textureLoad(emissiveTexture, pixelI, 0);
    outputPixels[pixelIndex].geometryNormal = textureLoad(geometryNormalTexture, pixelI, 0);
    outputPixels[pixelIndex].normal = textureLoad(normalTexture, pixelI, 0);
    outputPixels[pixelIndex].motionDepth = vec4<f32>(motion, textureLoad(depthTexture, pixelI, 0), 0.0);
}
