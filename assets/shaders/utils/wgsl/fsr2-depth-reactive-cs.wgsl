#include "fsr2-temporal-common.wgsl"

@group(0) @binding(0) var<uniform> uniforms : Fsr2Uniforms;
@group(0) @binding(1) var currentColorTexture : texture_2d<f32>;
@group(0) @binding(2) var dilatedDepthTexture : texture_2d<f32>;
@group(0) @binding(3) var dilatedMotionTexture : texture_2d<f32>;
@group(0) @binding(4) var inputDisocclusionMaskTexture : texture_2d<f32>;
@group(0) @binding(5) var historyDepthTexture : texture_2d<f32>;
@group(0) @binding(6) var historyDilatedMotionTexture : texture_2d<f32>;
@group(0) @binding(7) var depthClipTexture : texture_storage_2d<r32float, write>;
@group(0) @binding(8) var reactiveMasksTexture : texture_storage_2d<rgba16float, write>;
// Nearest current-frame depth reprojecting onto each previous-frame pixel; see
// the scatter in fsr2-reconstruct-dilate-cs.wgsl.
@group(0) @binding(9) var<storage, read> reconstructedDepth : array<u32>;

// Positive view distance from device depth. Exact for reverse-Z, normal Z and an
// infinite far plane, because the two projection-matrix entries are passed in
// rather than near and far being guessed at here.
fn view_distance(deviceDepth : f32) -> f32 {
    return uniforms.depthLinearB / (deviceDepth + uniforms.depthLinearA);
}

// Disocclusion from the reconstructed previous depth.
//
// The question is not "did depth change here" - camera motion alone answers that
// yes, which is why the stage this replaces was unusable. It is "does a nearer
// surface also claim the history I am about to read". If one does, that history
// describes the nearer surface, not this pixel, and reusing it is a ghost.
//
// Reads the *same* 2x2 footprint the scatter wrote and keeps the nearest depth
// across it, for two reasons. It matches the bilinear footprint the history
// resample actually reads, so a surface partially covering a previous pixel is
// still counted. And it removes any sensitivity to sub-slot addressing: a read
// landing one slot away from the write picks up a neighbouring surface, and on a
// silhouette that neighbour is nearer - indistinguishable from a real
// disocclusion. That defect measured as a 0.0175 mean clip on a *static* camera,
// where nothing can have been disoccluded at all.
//
// Comparing view distances rather than device depths is what makes the threshold
// mean something: device depth is severely non-linear, so a single epsilon is far
// too tight near the camera and inert in the distance.
//
// Returns zero when nothing was projected onto the footprint, which is the
// correct answer and not a missing one - no current-frame surface competes for
// that history, so there is nothing to reject.
fn reconstructed_disocclusion(previousPosition : vec2<f32>, currentDepth : f32) -> f32 {
    let reverse = uniforms.reverseDepth != 0u;
    let sentinel = reconstructed_depth_sentinel(reverse);
    let base = vec2<i32>(floor(previousPosition));
    let frac = previousPosition - floor(previousPosition);
    // Bilinear weights, matching the footprint the history resample reads.
    //
    // Weighted rather than nearest-of-four, and the difference is not cosmetic.
    // Taking the nearest depth across the footprint reaches into whatever
    // neighbouring surface the footprint touches, so every silhouette pixel finds
    // a nearer surface and reports a disocclusion - measured at a 0.0175 mean
    // clip on a *static* camera, where nothing can have been disoccluded, and it
    // raised instability rather than lowering it. Weighting by coverage means a
    // barely-touched neighbour contributes barely anything.
    let weights = vec4<f32>(
        (1.0 - frac.x) * (1.0 - frac.y),
        frac.x * (1.0 - frac.y),
        (1.0 - frac.x) * frac.y,
        frac.x * frac.y);
    let currentDistance = view_distance(currentDepth);
    let threshold = max(uniforms.depthSeparationScale, 1.0e-5);
    var clipSum = 0.0;
    var weightSum = 0.0;
    for (var tap = 0; tap < 4; tap++) {
        if (weights[tap] <= 0.01) {
            continue;
        }
        let slot = base + vec2<i32>(tap & 1, tap >> 1);
        if (!reconstructed_depth_in_bounds(slot, uniforms.sourceViewport)) {
            continue;
        }
        let stored = reconstructedDepth[
            reconstructed_depth_index(slot, uniforms.sourceViewport)];
        if (stored == sentinel) {
            continue;
        }
        // Only being *behind* the claimant is evidence. In front of it means this
        // pixel is the nearest one and owns the history, and such taps are left
        // out of the normalisation entirely rather than counted as agreement.
        let separation = currentDistance - view_distance(bitcast<f32>(stored));
        if (separation <= 0.0) {
            continue;
        }
        let relative = separation / max(currentDistance, 1.0e-4);
        clipSum += smoothstep(threshold * 0.5, threshold, relative) * weights[tap];
        weightSum += weights[tap];
    }
    if (weightSum <= 0.0) {
        return 0.0;
    }
    return clamp(clipSum / weightSum, 0.0, 1.0);
}

fn motion_divergence(pixel : vec2<i32>) -> f32 {
    let centerMotion = textureLoad(dilatedMotionTexture, pixel, 0).xy;
    let centerVelocity = length(centerMotion * viewport_f(uniforms.outputViewport));
    if (centerVelocity <= 1e-2) {
        return 0.0;
    }

    var minConvergence = 1.0;
    var maxVelocityUv = max(length(centerMotion), 1e-5);
    for (var y = -1; y <= 1; y++) {
        for (var x = -1; x <= 1; x++) {
            let samplePixel = clamp_pixel(pixel + vec2<i32>(x, y), uniforms.sourceViewport);
            let motion = textureLoad(dilatedMotionTexture, samplePixel, 0).xy;
            maxVelocityUv = max(maxVelocityUv, length(motion));
            minConvergence = min(minConvergence, dot(motion / max(maxVelocityUv, 1e-5), centerMotion / max(maxVelocityUv, 1e-5)));
        }
    }
    return clamp(1.0 - minConvergence, 0.0, 1.0) * clamp(maxVelocityUv / 0.01, 0.0, 1.0);
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) globalInvocationId : vec3<u32>) {
    if (globalInvocationId.x >= uniforms.sourceViewport.x || globalInvocationId.y >= uniforms.sourceViewport.y) {
        return;
    }

    let pixel = vec2<i32>(globalInvocationId.xy);
    let lrUv = uv_from_pixel(globalInvocationId.xy, uniforms.sourceViewport);
    let motion = textureLoad(dilatedMotionTexture, pixel, 0).xy;
    // lrUv is on the jittered source grid, while depth history is stored on
    // the stable display grid. Undo current jitter before applying velocity.
    let currentDisplayUv = lrUv -
        uniforms.currentJitter / viewport_f(uniforms.sourceViewport);
    let historyUv = currentDisplayUv - motion;
    let currentDepth = textureLoad(dilatedDepthTexture, pixel, 0).x;

    var depthClip = 1.0;
    if (uniforms.historyValid != 0u && is_uv_inside(historyUv)) {
        if (uniforms.reconstructedDepth != 0u) {
            // A background pixel still has no surface to reproject and its
            // motion vector is zero, so it would otherwise keep reading whatever
            // was drawn at this location many frames ago.
            if (is_background_depth(currentDepth, uniforms.reverseDepth != 0u)) {
                depthClip = 1.0;
            } else {
                depthClip = reconstructed_disocclusion(
                    reconstructed_depth_position(
                        pixel, motion, uniforms.currentJitter,
                        uniforms.sourceViewport),
                    currentDepth);
            }
        } else if (uniforms.stochasticInput != 0u) {
            // Device-space depth changes under camera translation even when
            // motion vectors reproject the same surface. For stochastic light
            // only reject a surface/background class change here; the
            // accumulate stage also validates the reprojected normal.
            let currentIsBackground = is_background_depth(currentDepth, uniforms.reverseDepth != 0u);
            if (currentIsBackground) {
                // A background pixel has no surface to reproject, so the
                // motion vector is always zero and this would otherwise keep
                // sampling whatever was drawn at this screen location many
                // frames ago. Always disocclude it.
                depthClip = 1.0;
            } else {
                // Surface/background class change only; see the matching
                // comment in fsr2-accumulate-cs for why depth magnitude is not
                // usable here.
                let historyDepth = textureLoad(
                    historyDepthTexture,
                    pixel_from_uv(historyUv, uniforms.outputViewport),
                    0).x;
                depthClip = select(
                    0.0,
                    1.0,
                    currentIsBackground != is_background_depth(historyDepth, uniforms.reverseDepth != 0u));
            }
        } else {
            let historyDepth = sample_catmull_rom_scalar(historyDepthTexture, historyUv, uniforms.outputViewport);
            let depthDelta = abs(currentDepth - historyDepth);
            depthClip = smoothstep(uniforms.depthThreshold * 0.5, uniforms.depthThreshold, depthDelta);
        }
    }

    // This mask is produced at source resolution, not display resolution.
    let inputDisocclusionPixel = pixel_from_uv(lrUv, uniforms.sourceViewport);
    let inputDisocclusion = clamp(textureLoad(inputDisocclusionMaskTexture, inputDisocclusionPixel, 0).x * uniforms.inputDisocclusionScale, 0.0, 1.0);
    let localMotionDivergence = motion_divergence(pixel);
    var temporalMotionDivergence = 0.0;
    if (uniforms.historyValid != 0u && is_uv_inside(historyUv)) {
        let previousSourceUv = source_uv_from_output_uv(
            historyUv, uniforms.previousJitter, uniforms.sourceViewport);
        let previousMotion = sample_catmull_rom_color(
            historyDilatedMotionTexture,
            previousSourceUv,
            uniforms.sourceViewport).xy;
        let motionDistance = length(motion * viewport_f(uniforms.outputViewport));
        temporalMotionDivergence = select(0.0, 1.0 - clamp(length(previousMotion) / max(length(motion), 1e-5), 0.0, 1.0), motionDistance > 1.0);
        temporalMotionDivergence *= clamp(pow(motionDistance / 20.0, 3.0), 0.0, 1.0);
    }

    let stability = clamp(uniforms.stabilityFactor, 0.0, 1.0);
    let reactive = clamp(max(inputDisocclusion, depthClip * mix(0.25, 0.12, stability)), 0.0, 1.0);
    let accumulationMask = select(
        clamp(max(localMotionDivergence, temporalMotionDivergence) * mix(1.0, 0.45, stability), 0.0, 1.0),
        0.0,
        uniforms.stochasticInput != 0u);

    textureStore(depthClipTexture, pixel, vec4<f32>(depthClip, 0.0, 0.0, 0.0));
    textureStore(reactiveMasksTexture, pixel, vec4<f32>(reactive, accumulationMask, 0.0, 0.0));
}
