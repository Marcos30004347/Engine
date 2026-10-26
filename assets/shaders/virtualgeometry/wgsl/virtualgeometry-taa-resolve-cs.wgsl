#include "virtualgeometry-taa-common.wgsl"

struct TaaResolveUniforms {
    sourceViewport : vec2<u32>,
    outputViewport : vec2<u32>,
    currentJitter : vec2<f32>,
    previousJitter : vec2<f32>,
    historyValid : u32,
    _padding0 : vec3<u32>,
    baseWeight : f32,
    motionScale : f32,
    depthThreshold : f32,
    normalThreshold : f32,
    reactiveScale : f32,
    edgeScale : f32,
    sharpenStrength : f32,
    detailBoost : f32,
}

@group(0) @binding(0) var<uniform> taaUniforms : TaaResolveUniforms;
@group(0) @binding(1) var linearSampler : sampler;
@group(0) @binding(2) var currentColorTexture : texture_2d<f32>;
@group(0) @binding(3) var currentGeometryNormalTexture : texture_2d<f32>;
@group(0) @binding(4) var sceneDepthTexture : texture_depth_2d;
@group(0) @binding(5) var motionVectorTexture : texture_2d<f32>;
@group(0) @binding(6) var packedGeometryIdsLoTexture : texture_2d<u32>;
@group(0) @binding(7) var packedGeometryIdsHiTexture : texture_2d<u32>;
@group(0) @binding(8) var historyColorTexture : texture_2d<f32>;
@group(0) @binding(9) var historyDepthTexture : texture_2d<f32>;
@group(0) @binding(10) var historyIdsTexture : texture_2d<u32>;
@group(0) @binding(16) var historyNormalTexture : texture_2d<f32>;
@group(0) @binding(11) var<storage, read> instances : array<InstanceData>;
@group(0) @binding(12) var<storage, read> pageTable : array<PageTableEntry>;
@group(0) @binding(13) var<storage, read> pagesBuffer : array<u32>;
@group(0) @binding(14) var outputTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(15) var disocclusionMaskTexture : texture_storage_2d<r16float, write>;

@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) globalInvocationId : vec3<u32>) {
    if (globalInvocationId.x >= taaUniforms.outputViewport.x || globalInvocationId.y >= taaUniforms.outputViewport.y) {
        return;
    }

    let outputPixelCoords = vec2<u32>(globalInvocationId.xy);
    let outputPixelCoordsI = vec2<i32>(outputPixelCoords);
    let outputViewportF = vec2<f32>(f32(taaUniforms.outputViewport.x), f32(taaUniforms.outputViewport.y));
    let outputUv = taa_uv_from_pixel(outputPixelCoords, taaUniforms.outputViewport);
    let sourceViewportF = vec2<f32>(f32(taaUniforms.sourceViewport.x), f32(taaUniforms.sourceViewport.y));
    let sourceUv = clamp(outputUv + taaUniforms.currentJitter / sourceViewportF, vec2<f32>(0.0), vec2<f32>(1.0));
    let sourceCenterPixel = taa_source_pixel_coords(sourceUv, taaUniforms.sourceViewport);

    var neighborhoodMin = vec3<f32>(1e9);
    var neighborhoodMax = vec3<f32>(-1e9);
    var neighborhoodMinYCoCg = vec3<f32>(1e9);
    var neighborhoodMaxYCoCg = vec3<f32>(-1e9);
    let currentFiltered = taa_sample_catmull_rom(currentColorTexture, sourceUv, taaUniforms.sourceViewport);
    let sourceCenterColor = textureLoad(currentColorTexture, sourceCenterPixel, 0);
    var neighborhoodSum = vec3<f32>(0.0);
    var neighborhoodSumYCoCg = vec3<f32>(0.0);
    var neighborhoodSumSqYCoCg = vec3<f32>(0.0);
    var depthMin = 1.0;
    var depthMax = 0.0;
    var closestDepth = -1.0;
    var closestSurfacePixel = sourceCenterPixel;

    for (var y = -1; y <= 1; y++) {
        for (var x = -1; x <= 1; x++) {
            let samplePixel = clamp(
                vec2<i32>(sourceCenterPixel.x + x, sourceCenterPixel.y + y),
                vec2<i32>(0, 0),
                vec2<i32>(i32(taaUniforms.sourceViewport.x) - 1, i32(taaUniforms.sourceViewport.y) - 1)
            );
            let sampleColor = textureLoad(currentColorTexture, samplePixel, 0);
            let sampleDepth = textureLoad(sceneDepthTexture, samplePixel, 0);
            let sampleColorYCoCg = taa_rgb_to_ycocg(sampleColor.rgb);

            neighborhoodMin = min(neighborhoodMin, sampleColor.rgb);
            neighborhoodMax = max(neighborhoodMax, sampleColor.rgb);
            neighborhoodMinYCoCg = min(neighborhoodMinYCoCg, sampleColorYCoCg);
            neighborhoodMaxYCoCg = max(neighborhoodMaxYCoCg, sampleColorYCoCg);
            neighborhoodSum += sampleColor.rgb;
            neighborhoodSumYCoCg += sampleColorYCoCg;
            neighborhoodSumSqYCoCg += sampleColorYCoCg * sampleColorYCoCg;
            depthMin = min(depthMin, sampleDepth);
            depthMax = max(depthMax, sampleDepth);

            if (sampleDepth > closestDepth) {
                closestDepth = sampleDepth;
                closestSurfacePixel = samplePixel;
            }
        }
    }

    let sourcePixelCoordsI = closestSurfacePixel;
    let sourcePixelCoords = vec2<u32>(sourcePixelCoordsI);
    let velocity = textureLoad(motionVectorTexture, sourcePixelCoordsI, 0).xy;
    let jitterVelocity = (taaUniforms.currentJitter - taaUniforms.previousJitter) / sourceViewportF;
    let resolvedVelocity = velocity - jitterVelocity;
    let historyUv = outputUv - resolvedVelocity;

    let currDepth = textureLoad(sceneDepthTexture, sourcePixelCoordsI, 0);
    let currLo = textureLoad(packedGeometryIdsLoTexture, sourcePixelCoordsI, 0).x;
    let currHi = textureLoad(packedGeometryIdsHiTexture, sourcePixelCoordsI, 0).x;
    let currIds = taa_unpack_geometry_ids(currLo, currHi);
    let currNormalSample = textureLoad(currentGeometryNormalTexture, sourcePixelCoordsI, 0).xyz;
    let currNormal = normalize(select(vec3<f32>(0.0, 0.0, 1.0), currNormalSample, length(currNormalSample) > 1e-5));
    let neighborhoodAvg = neighborhoodSum / 9.0;
    let neighborhoodAvgYCoCg = neighborhoodSumYCoCg / 9.0;
    let neighborhoodVarianceYCoCg = max(neighborhoodSumSqYCoCg / 9.0 - neighborhoodAvgYCoCg * neighborhoodAvgYCoCg, vec3<f32>(0.0));
    let neighborhoodSigmaYCoCg = sqrt(neighborhoodVarianceYCoCg);
    let varianceGamma = vec3<f32>(1.25, 1.75, 1.75);
    let clipBoxMinYCoCg = min(neighborhoodMinYCoCg, neighborhoodAvgYCoCg - neighborhoodSigmaYCoCg * varianceGamma);
    let clipBoxMaxYCoCg = max(neighborhoodMaxYCoCg, neighborhoodAvgYCoCg + neighborhoodSigmaYCoCg * varianceGamma);
    let localContrastVec = neighborhoodMax - neighborhoodMin;
    let localContrast = max(max(localContrastVec.x, localContrastVec.y), localContrastVec.z);
    let sharpenAttenuation = 1.0 - smoothstep(0.18, 0.75, localContrast);
    let detailDelta = sourceCenterColor.rgb - currentFiltered.rgb;
    let sharpenDelta = currentFiltered.rgb - neighborhoodAvg;
    let sharpenedRgb = currentFiltered.rgb
        + sharpenDelta * (taaUniforms.sharpenStrength * sharpenAttenuation)
        + detailDelta * taaUniforms.detailBoost;
    let sharpenSlack = max(localContrastVec * 0.25, vec3<f32>(0.01));
    let currentResolved = vec4<f32>(
        clamp(sharpenedRgb, neighborhoodMin - sharpenSlack, neighborhoodMax + sharpenSlack),
        currentFiltered.a
    );

    var outputColor = currentResolved;
    var disocclusionMask = 1.0;
    let historyOffscreen =
        historyUv.x < 0.0 || historyUv.y < 0.0
        || historyUv.x > 1.0 || historyUv.y > 1.0;

    if (taaUniforms.historyValid != 0u && !historyOffscreen) {
        let historyPixel = taa_pixel_coords(historyUv, taaUniforms.outputViewport);
        let historyColor = taa_sample_catmull_rom(historyColorTexture, historyUv, taaUniforms.outputViewport);
        let historyDepth = textureLoad(historyDepthTexture, historyPixel, 0).x;
        let historyIdsPacked = textureLoad(historyIdsTexture, historyPixel, 0).xy;
        let historyIds = taa_unpack_geometry_ids(historyIdsPacked.x, historyIdsPacked.y);

        let sameSurface = taa_same_surface(currIds, historyIds);
        let depthDelta = abs(currDepth - historyDepth);
        let motionPixels = length(resolvedVelocity * outputViewportF);
        let motionFactor = clamp(motionPixels / 0.35, 0.0, 1.0);
        let depthThreshold = max(taaUniforms.depthThreshold, 1e-6);
        let depthDisocclusion = select(
            1.0,
            smoothstep(depthThreshold * 0.5, depthThreshold, depthDelta),
            sameSurface
        );
        let depthConfidence = 1.0 - depthDisocclusion;
        let motionConfidence = 1.0 - clamp(motionPixels * taaUniforms.motionScale, 0.0, 1.0);

        let currentResolvedYCoCg = taa_rgb_to_ycocg(currentResolved.rgb);
        let historyColorYCoCg = taa_rgb_to_ycocg(historyColor.rgb);
        let clippedHistoryYCoCg = taa_clip_aabb(historyColorYCoCg, currentResolvedYCoCg, clipBoxMinYCoCg, clipBoxMaxYCoCg);
        let clippedHistoryRgb = taa_ycocg_to_rgb(clippedHistoryYCoCg);
        let clippedHistory = vec4<f32>(clippedHistoryRgb, historyColor.a);
        let reactiveDelta = abs(taa_luma(currentResolved.rgb) - taa_luma(clippedHistory.rgb));
        let reactiveFactor = 1.0 - clamp(reactiveDelta * taaUniforms.reactiveScale * motionFactor, 0.0, 1.0);
        disocclusionMask = depthDisocclusion;

        var historyWeight = taaUniforms.baseWeight;
        historyWeight *= depthConfidence;
        historyWeight *= motionConfidence;
        historyWeight *= reactiveFactor;
        historyWeight *= 1.0 - clamp(disocclusionMask * 1.25, 0.0, 1.0);

        if (!sameSurface || depthConfidence <= 0.0) {
            historyWeight = 0.0;
        }

        if (sameSurface && motionPixels <= 0.05 && depthConfidence > 0.92) {
            historyWeight = max(historyWeight, min(taaUniforms.baseWeight + 0.02, 0.94));
        }

        outputColor = mix(currentResolved, clippedHistory, clamp(historyWeight, 0.0, 1.0));
    }

    textureStore(outputTexture, outputPixelCoordsI, outputColor);
    textureStore(disocclusionMaskTexture, outputPixelCoordsI, vec4<f32>(clamp(disocclusionMask, 0.0, 1.0), 0.0, 0.0, 0.0));
}
