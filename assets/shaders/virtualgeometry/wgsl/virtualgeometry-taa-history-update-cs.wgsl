#include "virtualgeometry-taa-common.wgsl"

struct TaaHistoryUpdateUniforms {
    sourceViewport : vec2<u32>,
    outputViewport : vec2<u32>,
    currentJitter : vec2<f32>,
    _padding0 : vec2<f32>,
}

@group(0) @binding(0) var<uniform> taaUniforms : TaaHistoryUpdateUniforms;
@group(0) @binding(1) var resolvedColorTexture : texture_2d<f32>;
@group(0) @binding(2) var currentGeometryNormalTexture : texture_2d<f32>;
@group(0) @binding(3) var sceneDepthTexture : texture_depth_2d;
@group(0) @binding(4) var packedGeometryIdsLoTexture : texture_2d<u32>;
@group(0) @binding(12) var packedGeometryIdsHiTexture : texture_2d<u32>;
@group(0) @binding(5) var<storage, read> instances : array<InstanceData>;
@group(0) @binding(6) var<storage, read> pageTable : array<PageTableEntry>;
@group(0) @binding(7) var<storage, read> pagesBuffer : array<u32>;
@group(0) @binding(8) var historyColorTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(9) var historyDepthTexture : texture_storage_2d<r32float, write>;
@group(0) @binding(10) var historyIdsTexture : texture_storage_2d<rg32uint, write>;
@group(0) @binding(11) var historyNormalTexture : texture_storage_2d<rgba16float, write>;

@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) globalInvocationId : vec3<u32>) {
    if (globalInvocationId.x >= taaUniforms.outputViewport.x || globalInvocationId.y >= taaUniforms.outputViewport.y) {
        return;
    }

    let outputPixelCoords = vec2<u32>(globalInvocationId.xy);
    let outputPixelCoordsI = vec2<i32>(outputPixelCoords);
    let outputUv = taa_uv_from_pixel(outputPixelCoords, taaUniforms.outputViewport);
    let sourceViewportF = vec2<f32>(f32(taaUniforms.sourceViewport.x), f32(taaUniforms.sourceViewport.y));
    let sourceUv = clamp(outputUv + taaUniforms.currentJitter / sourceViewportF, vec2<f32>(0.0), vec2<f32>(1.0));
    let sourceCenterPixel = taa_source_pixel_coords(sourceUv, taaUniforms.sourceViewport);
    let resolvedColor = textureLoad(resolvedColorTexture, outputPixelCoordsI, 0);
    var closestDepth = -1.0;
    var sourcePixelCoordsI = sourceCenterPixel;

    for (var y = -1; y <= 1; y++) {
        for (var x = -1; x <= 1; x++) {
            let samplePixel = clamp(
                vec2<i32>(sourceCenterPixel.x + x, sourceCenterPixel.y + y),
                vec2<i32>(0, 0),
                vec2<i32>(i32(taaUniforms.sourceViewport.x) - 1, i32(taaUniforms.sourceViewport.y) - 1)
            );
            let sampleDepth = textureLoad(sceneDepthTexture, samplePixel, 0);
            if (sampleDepth > closestDepth) {
                closestDepth = sampleDepth;
                sourcePixelCoordsI = samplePixel;
            }
        }
    }

    let sourcePixelCoords = vec2<u32>(sourcePixelCoordsI);
    let depth = textureLoad(sceneDepthTexture, sourcePixelCoordsI, 0);
    let idsLo = textureLoad(packedGeometryIdsLoTexture, sourcePixelCoordsI, 0).x;
    let idsHi = textureLoad(packedGeometryIdsHiTexture, sourcePixelCoordsI, 0).x;
    let currentNormalSample = textureLoad(currentGeometryNormalTexture, sourcePixelCoordsI, 0).xyz;
    let currentNormal = normalize(select(vec3<f32>(0.0, 0.0, 1.0), currentNormalSample, length(currentNormalSample) > 1e-5));

    textureStore(historyColorTexture, outputPixelCoordsI, resolvedColor);
    textureStore(historyDepthTexture, outputPixelCoordsI, vec4<f32>(depth, 0.0, 0.0, 0.0));
    textureStore(historyIdsTexture, outputPixelCoordsI,
        vec4<u32>(taaStableInstanceToken(idsLo), idsHi, 0u, 0u));
    textureStore(historyNormalTexture, outputPixelCoordsI, vec4<f32>(currentNormal, 1.0));
}
