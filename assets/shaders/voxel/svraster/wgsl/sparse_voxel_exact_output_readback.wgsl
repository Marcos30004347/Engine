// R0 exact-quality capture.
//
// Keep upstream visibility/material identity beside the attachment-quantized
// G-buffer. A final G-buffer hash alone cannot tell an optimization that
// changed temporal identity, the packed bottom layer, a micro payload, or the
// canonical material-field address where two changes happen to shade alike.
#include "sparse_voxel_types.wgsl"

struct SparseVoxelExactOutputPixel {
    surface : ResolvedVoxelSurface,
    materialSampleAddress : VoxelMaterialSampleAddress,
    // x: micro payload, y: temporal identity, z: coverage, w: reserved.
    diagnostics : vec4<u32>,
    gbuffer : SparseVoxelGBufferPixel,
}

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
@group(0) @binding(12) var temporalIdTexture : texture_2d<u32>;
@group(0) @binding(13) var<storage, read> resolvedSurfaces :
    array<ResolvedVoxelSurface>;
@group(0) @binding(14) var<storage, read> microPayloads : array<u32>;
@group(0) @binding(15) var<storage, read> materialSampleAddresses :
    array<VoxelMaterialSampleAddress>;
@group(0) @binding(16) var<storage, read> coverage : array<u32>;
@group(0) @binding(17) var<storage, read_write> outputPixels :
    array<SparseVoxelExactOutputPixel>;

@compute @workgroup_size(8, 8, 1)
fn readSparseVoxelExactOutput(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let pixel = globalId.xy;
    if (any(pixel >= uniforms.viewportTiles.xy)) {
        return;
    }
    let pixelI = vec2<i32>(pixel);
    let pixelIndex = pixel.y * uniforms.viewportTiles.x + pixel.x;
    let materialUv = textureLoad(materialUvTexture, pixelI, 0).xy;
    let motion = textureLoad(motionVectorTexture, pixelI, 0).xy;

    outputPixels[pixelIndex].surface = resolvedSurfaces[pixelIndex];
    outputPixels[pixelIndex].materialSampleAddress =
        materialSampleAddresses[pixelIndex];
    outputPixels[pixelIndex].diagnostics = vec4<u32>(
        microPayloads[pixelIndex],
        textureLoad(temporalIdTexture, pixelI, 0).x,
        coverage[pixelIndex], 0u);
    outputPixels[pixelIndex].gbuffer.ids = vec4<u32>(
        textureLoad(instanceIdTexture, pixelI, 0).x,
        textureLoad(voxelIdTexture, pixelI, 0).x,
        textureLoad(materialIdTexture, pixelI, 0).x,
        0u);
    outputPixels[pixelIndex].gbuffer.materialUv =
        vec4<f32>(materialUv, 0.0, 0.0);
    outputPixels[pixelIndex].gbuffer.baseColor =
        textureLoad(baseColorTexture, pixelI, 0);
    outputPixels[pixelIndex].gbuffer.materialProperties =
        textureLoad(materialPropertiesTexture, pixelI, 0);
    outputPixels[pixelIndex].gbuffer.emissive =
        textureLoad(emissiveTexture, pixelI, 0);
    outputPixels[pixelIndex].gbuffer.geometryNormal =
        textureLoad(geometryNormalTexture, pixelI, 0);
    outputPixels[pixelIndex].gbuffer.normal =
        textureLoad(normalTexture, pixelI, 0);
    outputPixels[pixelIndex].gbuffer.motionDepth = vec4<f32>(
        motion, textureLoad(depthTexture, pixelI, 0), 0.0);
}
