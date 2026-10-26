#include "fsr2-temporal-common.wgsl"

@group(0) @binding(0) var<uniform> uniforms : Fsr2Uniforms;
@group(0) @binding(1) var lockInputLumaTexture : texture_2d<f32>;
@group(0) @binding(2) var dilatedMotionTexture : texture_2d<f32>;
@group(0) @binding(3) var historyLockStatusTexture : texture_2d<f32>;
@group(0) @binding(4) var lockStatusScratchTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(5) var newLocksTexture : texture_storage_2d<r32float, write>;

fn source_uv_for_output_pixel(outputPixel : vec2<i32>) -> vec2<f32> {
    let outputUv = (vec2<f32>(outputPixel) + vec2<f32>(0.5)) / viewport_f(uniforms.outputViewport);
    return source_uv_from_output_uv(outputUv, uniforms.currentJitter, uniforms.sourceViewport);
}

fn load_luma_hr(outputPixel : vec2<i32>) -> f32 {
    return sample_catmull_rom_scalar(
        lockInputLumaTexture, source_uv_for_output_pixel(outputPixel), uniforms.sourceViewport);
}

// The nucleus is passed in rather than re-fetched: main() already needs it as
// currentLuma, and it used to be sampled a second time here.
//
// The eight neighbours use a four-tap bilinear instead of catmull-rom. This
// loop is the dominant cost of the whole lock stage - ten catmull-rom samples
// is 160 texture fetches per output pixel - and it only ever compares relative
// luma magnitudes to find a thin ridge, which catmull-rom's extra taps and
// negative lobes do not make more reliable.
fn compute_thin_feature_confidence(outputPixel : vec2<i32>, nucleus : f32) -> bool {
    let similarThreshold = 1.05;
    var similarCount = 0u;
    var dissimilarMin = 1e9;
    var dissimilarMax = -1e9;

    for (var y = -1; y <= 1; y++) {
        for (var x = -1; x <= 1; x++) {
            let samplePixel = clamp_pixel(outputPixel + vec2<i32>(x, y), uniforms.outputViewport);
            let sampleLuma = select(
                sample_bilinear_scalar(
                    lockInputLumaTexture,
                    source_uv_for_output_pixel(samplePixel),
                    uniforms.sourceViewport),
                nucleus,
                x == 0 && y == 0);
            let ratio = max(sampleLuma, nucleus) / max(min(sampleLuma, nucleus), 1e-5);
            if (ratio < similarThreshold) {
                similarCount += 1u;
            } else {
                dissimilarMin = min(dissimilarMin, sampleLuma);
                dissimilarMax = max(dissimilarMax, sampleLuma);
            }
        }
    }

    let isRidge = nucleus > dissimilarMax || nucleus < dissimilarMin;
    return isRidge && similarCount <= 5u;
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) globalInvocationId : vec3<u32>) {
    if (globalInvocationId.x >= uniforms.outputViewport.x || globalInvocationId.y >= uniforms.outputViewport.y) {
        return;
    }

    let outputPixel = vec2<i32>(globalInvocationId.xy);
    let outputUv = uv_from_pixel(globalInvocationId.xy, uniforms.outputViewport);
    let sourceUv = source_uv_from_output_uv(outputUv, uniforms.currentJitter, uniforms.sourceViewport);
    let motion = sample_catmull_rom_color(dilatedMotionTexture, sourceUv, uniforms.sourceViewport).xy;
    let historyUv = outputUv - motion;
    let currentLuma = load_luma_hr(outputPixel);
    let newLock = select(0.0, 1.0, compute_thin_feature_confidence(outputPixel, currentLuma));

    var lockStatus = vec2<f32>(0.0, currentLuma);
    if (uniforms.historyValid != 0u && is_uv_inside(historyUv)) {
        lockStatus = sample_catmull_rom_color(historyLockStatusTexture, historyUv, uniforms.outputViewport).xy;
    }

    let lumaDiff = 1.0 - min_divided_by_max(lockStatus.y, currentLuma);
    if (newLock > 0.5) {
        lockStatus.x = select(1.0, 2.0, lockStatus.x > 0.0);
        lockStatus.y = currentLuma;
    } else if (lockStatus.x <= 1.0) {
        lockStatus.y = mix(lockStatus.y, currentLuma, 0.5);
    } else if (lumaDiff > 0.1) {
        lockStatus.x = 0.0;
    }

    textureStore(lockStatusScratchTexture, outputPixel, vec4<f32>(lockStatus, 0.0, 0.0));
    textureStore(newLocksTexture, outputPixel, vec4<f32>(newLock, 0.0, 0.0, 0.0));
}
