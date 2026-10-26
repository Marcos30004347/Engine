#include "fsr2-temporal-common.wgsl"

// Auto-exposure, which the pipeline had no equivalent of at all.
//
// FSR computes an exposure from the frame's own luminance and applies it before
// tonemapping, and every stage downstream is calibrated on the assumption that
// the values it sees have been through that. Skipping it is not a small
// simplification. `tonemap_for_accumulation` maps a linear 12 - this renderer's
// finite lights ship at intensity 12 - to 0.923, and 100 to 0.990: every lit
// value in the scene collapses into the top few percent of the range, where the
// rectification box, the history clip and the Lanczos kernel can no longer tell
// any of them apart. That is the most likely reason range-compressed
// accumulation measured as doing nothing useful on its own.
//
// Two entry points because they are one algorithm split by a barrier: a
// reduction cannot read its own total.
@group(0) @binding(0) var<uniform> uniforms : Fsr2Uniforms;
@group(0) @binding(1) var currentColorTexture : texture_2d<f32>;
@group(0) @binding(2) var<storage, read_write> exposure : array<atomic<i32>>;

// One partial sum per workgroup rather than one atomic per pixel. At source
// resolution that is a 64x reduction in contention on a single address, which
// otherwise serialises the whole dispatch.
var<workgroup> partialSum : array<f32, 64>;
var<workgroup> partialCount : array<u32, 64>;

@compute @workgroup_size(8, 8, 1)
fn reduceExposure(
    @builtin(global_invocation_id) globalInvocationId : vec3<u32>,
    @builtin(local_invocation_index) localIndex : u32) {
    var logLuminance = 0.0;
    var counted = 0u;
    if (globalInvocationId.x < uniforms.sourceViewport.x &&
        globalInvocationId.y < uniforms.sourceViewport.y) {
        let color = max(
            textureLoad(currentColorTexture, vec2<i32>(globalInvocationId.xy), 0).rgb,
            vec3<f32>(0.0));
        let luminance = luma(color);
        // A geometric mean, so the sum is over logarithms - which is what makes
        // the result track the scene's middle rather than being dragged to the
        // brightest pixel in frame. Black pixels are excluded rather than
        // clamped: log of zero is unbounded below, and a mostly-empty frame
        // would otherwise drive the exposure to nothing.
        if (luminance > EXPOSURE_LUMINANCE_FLOOR) {
            logLuminance = clamp(log(luminance), -15.0, 15.0);
            counted = 1u;
        }
    }
    partialSum[localIndex] = logLuminance;
    partialCount[localIndex] = counted;
    workgroupBarrier();

    // Tree reduction within the workgroup.
    for (var stride = 32u; stride > 0u; stride >>= 1u) {
        if (localIndex < stride) {
            partialSum[localIndex] += partialSum[localIndex + stride];
            partialCount[localIndex] += partialCount[localIndex + stride];
        }
        workgroupBarrier();
    }

    if (localIndex == 0u && partialCount[0] > 0u) {
        // Fixed point because there is no atomic float add. The scale is bounded
        // by the worst case sum: a 4K source is 8.3M pixels, times a log
        // magnitude of 15, so 256 keeps the total inside a signed 32-bit integer
        // while still resolving the mean to well under a stop.
        atomicAdd(&exposure[EXPOSURE_SLOT_SUM],
            i32(round(partialSum[0] * EXPOSURE_FIXED_SCALE)));
        atomicAdd(&exposure[EXPOSURE_SLOT_COUNT], i32(partialCount[0]));
    }
}

// Photographic exposure from the geometric mean luminance, following FSR's
// derivation: treat the mean as a metered value at ISO 100 with a reflected-light
// calibration constant, and return the reciprocal of the resulting saturation
// luminance.
@compute @workgroup_size(1, 1, 1)
fn resolveExposure(@builtin(global_invocation_id) globalInvocationId : vec3<u32>) {
    if (globalInvocationId.x != 0u) {
        return;
    }
    let count = atomicLoad(&exposure[EXPOSURE_SLOT_COUNT]);
    var value = 1.0;
    if (count > 0) {
        let meanLog = f32(atomicLoad(&exposure[EXPOSURE_SLOT_SUM])) /
            (EXPOSURE_FIXED_SCALE * f32(count));
        let meanLuminance = exp(meanLog);
        let speed = 100.0;
        let calibration = 12.5;
        let exposureIso100 = log2((meanLuminance * speed) / calibration);
        let q = 0.65;
        let saturationLuminance = (78.0 / (q * speed)) * exp2(exposureIso100);
        value = 1.0 / max(saturationLuminance, 1.0e-6);
    }
    // Bounded so one degenerate frame - a camera inside geometry, a frame before
    // any lighting has resolved - cannot blow the whole chain out. Without this
    // an exposure of zero silently turns every later stage into a no-op on black.
    atomicStore(
        &exposure[EXPOSURE_SLOT_VALUE],
        bitcast<i32>(clamp(value, EXPOSURE_MINIMUM, EXPOSURE_MAXIMUM)));
}
