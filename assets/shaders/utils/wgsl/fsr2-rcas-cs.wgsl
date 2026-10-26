#include "fsr2-temporal-common.wgsl"

@group(0) @binding(0) var<uniform> uniforms : Fsr2Uniforms;
@group(0) @binding(1) var internalColorTexture : texture_2d<f32>;
@group(0) @binding(2) var outputTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(3) var<storage, read> exposureSlots : array<i32>;

fn load_internal(pixel : vec2<i32>) -> vec3<f32> {
    return textureLoad(internalColorTexture, clamp_pixel(pixel, uniforms.outputViewport), 0).rgb;
}

// RCAS's own luma weighting, which is 2x a conventional luma: r/2 + g + b/2.
// The factor of two is absorbed by the ratios it is used in, so it is kept
// rather than normalised - every threshold below is calibrated against it.
fn rcas_luma(color : vec3<f32>) -> f32 {
    return color.b * 0.5 + (color.r * 0.5 + color.g);
}

// Robust Contrast Adaptive Sharpening.
//
// What was here before was a plain unsharp mask - a fixed high-pass added back
// with a contrast gate - and it differs from RCAS in the two ways that decide
// whether sharpening helps or hurts a temporally upscaled image.
//
// 1. AN UNSHARP MASK CANNOT BOUND ITS OWN OVERSHOOT. It adds a fixed multiple of
//    the high-pass whatever the local signal is doing, so on a hard edge it
//    manufactures values outside the neighbourhood's range - haloing - and on a
//    nearly-flat neighbourhood it multiplies up whatever is there, which after a
//    0.5-scale reconstruction is resampling residual rather than detail. That
//    residual changes every frame, so amplifying it is directly the "jitter is
//    noticeable" complaint. RCAS instead *solves* for the largest sharpening
//    filter that still cannot exceed the local min and max (`hitMin`/`hitMax`
//    below), which is the whole point of the "robust" in its name: the strength
//    is derived per pixel from what the neighbourhood can absorb, not dialled in.
//
// 2. THE OLD PATH BLURRED THE IMAGE UNCONDITIONALLY. `stableBase` mixed the
//    centre tap toward a box blur by `0.22 * stabilityFactor` - 15% of a box
//    blur at the shipping stability of 0.70 - and then sharpened *that*. Costing
//    the final image 15% of a blur to stabilise it is the opposite of what a
//    0.5-scale render can afford, and RCAS's own noise term does that job
//    without touching the signal: `noise` measures how far the centre departs
//    from its ring relative to the ring's spread and scales the sharpening down
//    where the answer is "not much", which is what an isolated speckle looks
//    like. That is FSR's `FSR_RCAS_DENOISE`, and it attenuates the *filter*
//    rather than the picture.
//
// RCAS's limiter is derived assuming the signal is bounded by one - `1.0 - mx4`
// is meaningless otherwise, and this renderer's image is linear HDR with lights
// at intensity 12, where that term goes negative and the limiter inverts. So the
// cross is range-compressed here regardless of `tonemapAccumulation`, sharpened,
// and expanded again. This is not the same decision as the accumulator's: there
// it is a choice about where the arithmetic is better conditioned, here it is a
// precondition of the algorithm. Sharpening in a perceptual space is also the
// behaviour wanted - a fixed linear lobe would sharpen a highlight far harder
// than a shadow at equal visible contrast.
fn rcas_sharpen(pixel : vec2<i32>, exposureValue : f32) -> vec3<f32> {
    // Exposure applies here too: the limiter's [0,1) precondition is only
    // meaningful once the image occupies that range rather than saturating in it.
    let expose = exposureValue;
    // RCAS reads a five-tap cross and nothing else:
    //      b
    //    d e f
    //      h
    let b = tonemap_for_accumulation(load_internal(pixel + vec2<i32>(0, -1)) * expose);
    let d = tonemap_for_accumulation(load_internal(pixel + vec2<i32>(-1, 0)) * expose);
    let e = tonemap_for_accumulation(load_internal(pixel) * expose);
    let f = tonemap_for_accumulation(load_internal(pixel + vec2<i32>(1, 0)) * expose);
    let h = tonemap_for_accumulation(load_internal(pixel + vec2<i32>(0, 1)) * expose);

    let bL = rcas_luma(b);
    let dL = rcas_luma(d);
    let eL = rcas_luma(e);
    let fL = rcas_luma(f);
    let hL = rcas_luma(h);

    // Noise detection. How far the centre departs from the mean of its ring,
    // as a fraction of the spread across all five taps. Near one for an
    // isolated pixel that its neighbours do not corroborate, near zero on a
    // genuine edge where the ring itself carries the contrast.
    let ringMean = 0.25 * (bL + dL + fL + hL);
    let lumaMax = max(max(max(bL, dL), max(eL, fL)), hL);
    let lumaMin = min(min(min(bL, dL), min(eL, fL)), hL);
    var noise = clamp(
        abs(ringMean - eL) / max(lumaMax - lumaMin, 1.0e-5), 0.0, 1.0);
    noise = 1.0 - 0.5 * noise;

    // Ring extremes per channel. The centre tap is deliberately excluded: the
    // limiter's question is what the *surround* can absorb, and including the
    // pixel being sharpened would let a speckle license its own amplification.
    let mn4 = min(min(b, d), min(f, h));
    let mx4 = max(max(b, d), max(f, h));

    // The two limits, in units of the surround weight. `hitMin` is how far the
    // filter may go before driving a channel below the ring minimum, `hitMax`
    // before driving it above one. Both are guarded rather than trusted: mx4 is
    // zero on a black neighbourhood and the `hitMax` denominator reaches zero
    // only for a saturated one, but a single NaN here propagates to the
    // presented image.
    let hitMin = mn4 / max(4.0 * mx4, vec3<f32>(1.0e-5));
    let hitMax = (vec3<f32>(1.0) - mx4) /
        min(4.0 * mn4 - vec3<f32>(4.0), vec3<f32>(-1.0e-5));
    let lobeRgb = max(-hitMin, hitMax);
    // Every term above is non-positive, and the sharpening lobe is a negative
    // surround weight. FSR caps it at 0.25 - 1/16; going past that sharpens
    // beyond what the four-tap kernel can represent without ringing.
    var lobe = max(
        -0.1875,
        min(max(max(lobeRgb.r, lobeRgb.g), lobeRgb.b), 0.0));
    lobe *= clamp(uniforms.sharpenStrength, 0.0, 1.0) * noise;

    let sharpened = (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);
    return inverse_tonemap_for_accumulation(sharpened) / max(expose, 1.0e-6);
}

// The unsharp mask this pass used to be, kept so the two can be compared on the
// same frame. See `robustSharpen`.
fn unsharp_mask(pixel : vec2<i32>) -> vec3<f32> {
    let b = load_internal(pixel + vec2<i32>(0, -1));
    let d = load_internal(pixel + vec2<i32>(-1, 0));
    let e = load_internal(pixel);
    let f = load_internal(pixel + vec2<i32>(1, 0));
    let h = load_internal(pixel + vec2<i32>(0, 1));

    let min4 = min(min(b, d), min(f, h));
    let max4 = max(max(b, d), max(f, h));
    let peak = max(max(max4.r, max4.g), max4.b);
    let trough = min(min(min4.r, min4.g), min4.b);
    let contrast = peak - trough;
    let stability = clamp(uniforms.stabilityFactor, 0.0, 1.0);
    let blurred = (e * 4.0 + b + d + f + h) * 0.125;
    let stableBase = mix(e, blurred, 0.22 * stability);
    let noiseLimiter = 1.0 - smoothstep(0.0, 0.08, abs(luma(e) - (luma(b) + luma(d) + luma(f) + luma(h)) * 0.25));
    let sharpness = uniforms.sharpenStrength * mix(1.0, 0.20, stability) * mix(1.0, 0.35, noiseLimiter) * smoothstep(0.01, 0.35, contrast);
    let highpass = stableBase * 4.0 - (b + d + f + h);
    return stableBase + highpass * (sharpness * 0.125);
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) globalInvocationId : vec3<u32>) {
    if (globalInvocationId.x >= uniforms.outputViewport.x || globalInvocationId.y >= uniforms.outputViewport.y) {
        return;
    }

    let pixel = vec2<i32>(globalInvocationId.xy);
    var color = vec3<f32>(0.0);
    if (uniforms.robustSharpen != 0u) {
        color = rcas_sharpen(
            pixel, frame_exposure(exposureSlots[EXPOSURE_SLOT_VALUE], uniforms.exposureEnabled));
    } else {
        color = unsharp_mask(pixel);
    }
    textureStore(outputTexture, pixel, vec4<f32>(max(color, vec3<f32>(0.0)), 1.0));
}
