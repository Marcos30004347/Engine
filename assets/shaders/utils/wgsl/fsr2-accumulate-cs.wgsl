#include "fsr2-temporal-common.wgsl"

struct SurfaceSample {
    sourceUv : vec2<f32>,
    pixel : vec2<i32>,
    depth : f32,
    normal : vec3<f32>,
    motion : vec2<f32>,
    identity : vec2<u32>,
}

@group(0) @binding(0) var<uniform> uniforms : Fsr2Uniforms;
@group(0) @binding(1) var preparedColorTexture : texture_2d<f32>;
@group(0) @binding(2) var dilatedDepthTexture : texture_2d<f32>;
@group(0) @binding(3) var dilatedMotionTexture : texture_2d<f32>;
@group(0) @binding(4) var depthClipTexture : texture_2d<f32>;
@group(0) @binding(5) var reactiveMasksTexture : texture_2d<f32>;
@group(0) @binding(6) var currentNormalTexture : texture_2d<f32>;
@group(0) @binding(7) var historyColorTexture : texture_2d<f32>;
@group(0) @binding(8) var historyDepthTexture : texture_2d<f32>;
@group(0) @binding(9) var historyNormalTexture : texture_2d<f32>;
@group(0) @binding(10) var lockStatusScratchTexture : texture_2d<f32>;
@group(0) @binding(11) var historyLumaTexture : texture_2d<f32>;
@group(0) @binding(12) var newLocksTexture : texture_2d<f32>;
// Kernel weight this pixel's surface has accumulated over the frames it has
// been tracked for, reprojected like every other history quantity. See the
// blend at the bottom of main().
@group(0) @binding(13) var historyAccumulationTexture : texture_2d<f32>;
@group(0) @binding(14) var outputHistoryColorTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(15) var outputHistoryDepthTexture : texture_storage_2d<r32float, write>;
@group(0) @binding(16) var outputHistoryNormalTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(17) var outputLumaTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(18) var outputLockStatusTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(19) var outputDisocclusionMaskTexture : texture_storage_2d<r16float, write>;
// Geometry identity of the surface each pixel resolved, at source resolution
// for the current frame and at output resolution for history. Depth and normal
// alone cannot separate two adjacent voxels: voxel faces are axis-aligned so a
// whole wall shares one normal, and reverse-Z device depth is deliberately not
// usable as a rejection signal here (it moves under pure camera translation
// even when the motion vector reprojects the same surface). Identity is the
// only signal that distinguishes "same surface, sub-pixel shift" from "a
// different voxel now covers this pixel".
@group(0) @binding(20) var currentInstanceTexture : texture_2d<u32>;
@group(0) @binding(21) var currentVoxelTexture : texture_2d<u32>;
@group(0) @binding(22) var historyIdTexture : texture_2d<u32>;
@group(0) @binding(23) var outputHistoryIdTexture : texture_storage_2d<rg32uint, write>;
// Diagnostic only, written when uniforms.debugTerms is set. `baseAccumulation`
// should keep ~0.97 of the history every frame; anything that discards it does
// so through one of the multiplicative factors between that line and the final
// mix, and inspecting the code did not say which. These two targets carry the
// whole chain so each link can be reduced to a screen mean and compared.
//   A: |motion.x| px, disocclusionFactor, reactiveFactor, motionFactor
//   B: historyWeightPreLock, lockContribution, historyWeight, |motion.y| px
// baseAccumulation and disocclusion had the other two slots; both were dropped
// once measured. baseAccumulation is a constant of the uniforms (0.9727 at the
// shipping settings) and disocclusion only restates disocclusionFactor, whereas
// the per-axis motion residual is what the measurement turned out to need. The
// two motion channels are pixels clamped to one, which keeps their full-screen
// sum inside the same fixed-point budget as the [0,1] factors.
@group(0) @binding(24) var outputDebugTermsATexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(25) var outputDebugTermsBTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(26) var outputAccumulationTexture : texture_storage_2d<r16float, write>;
@group(0) @binding(27) var<storage, read> exposureSlots : array<i32>;
// Coarse log-luminance grid, this frame and last. Sampled at a point rather than
// per cell so the signal varies smoothly across a cell boundary instead of
// stepping; a hard cell edge would show as a visible 16-pixel grid in whatever it
// modulates.
@group(0) @binding(28) var coarseLumaTexture : texture_2d<f32>;
@group(0) @binding(29) var historyCoarseLumaTexture : texture_2d<f32>;

// Coarse-scale lighting change at this pixel, reprojected.
//
// The previous frame's grid is read at the *reprojected* position, not at this
// pixel's own: without that, camera motion slides the whole grid under every pixel
// and the entire screen reports a shading change whenever the camera moves - which
// is precisely the failure the geometric terms already avoid by reprojecting.
fn pixel_shading_change(sourceUv : vec2<f32>, historyUv : vec2<f32>) -> f32 {
    let extent = coarse_luma_extent(uniforms.sourceViewport);
    let current = sample_bilinear_scalar(coarseLumaTexture, sourceUv, extent);
    let previous = sample_bilinear_scalar(historyCoarseLumaTexture, historyUv, extent);
    return shading_change(
        current, previous, uniforms.shadingChangeStrength,
        uniforms.shadingChangeStops);
}

fn sample_prepared(pixel : vec2<i32>) -> vec3<f32> {
    return textureLoad(preparedColorTexture, clamp_pixel(pixel, uniforms.sourceViewport), 0).xyz;
}

fn sample_identity(pixel : vec2<i32>) -> vec2<u32> {
    let clamped = clamp_pixel(pixel, uniforms.sourceViewport);
    return vec2<u32>(
        textureLoad(currentInstanceTexture, clamped, 0).x,
        textureLoad(currentVoxelTexture, clamped, 0).x);
}

// History identity is point sampled: an interpolated id is meaningless.
fn load_history_identity(pixel : vec2<i32>) -> vec2<u32> {
    return textureLoad(historyIdTexture, clamp_pixel(pixel, uniforms.outputViewport), 0).xy;
}

// Whether the surface under this pixel is present anywhere in the bilinear
// footprint the history resample will read from.
//
// A hard test against the single nearest history texel would reject on every
// sub-pixel reprojection along a voxel boundary, which under continuous motion
// is most of the screen - that trades blur for a total loss of accumulation.
// Accepting the surface when any of the four taps carries the same identity
// keeps history exactly while the surface is still being reprojected onto, and
// rejects it the frame the surface genuinely leaves this pixel's footprint.
// The search is one texel wider than the bilinear footprint, and that extra
// texel is the difference between a test this content can pass and one it
// cannot.
//
// The history stores a single identity per pixel, but a pixel whose footprint
// straddles a voxel boundary does not *have* a single identity - jitter lands
// its sample on voxel A one frame and voxel B the next, and the accumulated
// colour is legitimately a mixture of both. That mixture is the anti-aliasing.
// Against a 2x2 footprint the alternate voxel routinely appears in none of the
// four taps, so the test reports a disocclusion on a static camera where
// nothing can possibly have been disoccluded: measured at 0.19% of the screen
// rejecting outright every frame, and exactly 0.00% with jitter off.
//
// A 3x3 search asks the weaker question the single-id history can actually
// answer - is this surface anywhere near this pixel in the previous frame -
// and a surface that is genuinely disoccluded is absent from all nine.
fn identity_disocclusion(historyUv : vec2<f32>, currentIdentity : vec2<u32>) -> f32 {
    if (uniforms.identityRejection == 0u) {
        return 0.0;
    }
    let historyPos = historyUv * viewport_f(uniforms.outputViewport) - vec2<f32>(0.5);
    let base = vec2<i32>(floor(historyPos));
    for (var y = -1; y <= 2; y++) {
        for (var x = -1; x <= 2; x++) {
            if (all(load_history_identity(base + vec2<i32>(x, y)) == currentIdentity)) {
                return 0.0;
            }
        }
    }
    return 1.0;
}

fn sample_depth(pixel : vec2<i32>) -> f32 {
    return textureLoad(dilatedDepthTexture, clamp_pixel(pixel, uniforms.sourceViewport), 0).x;
}

fn sample_normal(pixel : vec2<i32>) -> vec3<f32> {
    let normalSample = textureLoad(currentNormalTexture, clamp_pixel(pixel, uniforms.sourceViewport), 0).xyz;
    return normalize(select(vec3<f32>(0.0, 0.0, 1.0), normalSample, length(normalSample) > 1e-5));
}

fn find_surface(outputUv : vec2<f32>) -> SurfaceSample {
    let sourceUv = source_uv_from_output_uv(outputUv, uniforms.currentJitter, uniforms.sourceViewport);
    let sourcePos = sourceUv * viewport_f(uniforms.sourceViewport) - vec2<f32>(0.5);
    let centerPixel = clamp_pixel(vec2<i32>(round(sourcePos)), uniforms.sourceViewport);
    var bestPixel = centerPixel;
    var bestDepth = sample_depth(centerPixel);
    for (var y = -1; y <= 1; y++) {
        for (var x = -1; x <= 1; x++) {
            let samplePixel = clamp_pixel(centerPixel + vec2<i32>(x, y), uniforms.sourceViewport);
            let depth = sample_depth(samplePixel);
            if (is_closer_depth(depth, bestDepth, uniforms.reverseDepth != 0u)) {
                bestDepth = depth;
                bestPixel = samplePixel;
            }
        }
    }

    var result : SurfaceSample;
    result.sourceUv = sourceUv;
    result.pixel = bestPixel;
    result.depth = bestDepth;
    result.normal = sample_normal(bestPixel);
    result.identity = sample_identity(bestPixel);
    // Reconstruct/dilate has already removed projection jitter so depth,
    // reactive-mask generation and accumulation all use identical motion.
    result.motion = textureLoad(dilatedMotionTexture, bestPixel, 0).xy;
    return result;
}

// Detect the narrow set of pixels whose source footprint crosses a voxel,
// face, or silhouette boundary. A jittered point sample legitimately chooses
// different sides of this boundary on different frames; treating that choice
// as a full disocclusion makes the edge appear and disappear instead of
// converging to fractional coverage.
fn surface_edge_factor(surface : SurfaceSample) -> f32 {
    let centerBackground = is_background_depth(
        surface.depth, uniforms.reverseDepth != 0u);
    var transitions = 0u;
    for (var y = -1; y <= 1; y++) {
        for (var x = -1; x <= 1; x++) {
            if (x == 0 && y == 0) {
                continue;
            }
            let pixel = clamp_pixel(
                surface.pixel + vec2<i32>(x, y), uniforms.sourceViewport);
            let depth = sample_depth(pixel);
            let background = is_background_depth(
                depth, uniforms.reverseDepth != 0u);
            let identityChanged = any(sample_identity(pixel) != surface.identity);
            let normalChanged = dot(sample_normal(pixel), surface.normal) < 0.65;
            if (background != centerBackground || identityChanged || normalChanged) {
                transitions += 1u;
            }
        }
    }
    // Two transitioning neighbours are enough to describe a sub-pixel edge;
    // one receives half strength so isolated noisy ids do not soften a surface.
    return clamp(f32(transitions) * 0.5, 0.0, 1.0);
}

fn rectify_add_sample(initial : bool, box : ptr<function, RectificationBox>, colorYCoCg : vec3<f32>, weight : f32) {
    if (initial) {
        (*box).aabbMin = colorYCoCg;
        (*box).aabbMax = colorYCoCg;
        (*box).boxCenter = colorYCoCg * weight;
        (*box).boxVec = colorYCoCg * colorYCoCg * weight;
    } else {
        (*box).aabbMin = min((*box).aabbMin, colorYCoCg);
        (*box).aabbMax = max((*box).aabbMax, colorYCoCg);
        (*box).boxCenter += colorYCoCg * weight;
        (*box).boxVec += colorYCoCg * colorYCoCg * weight;
    }
}

fn compute_upsampled(outputPixel : vec2<i32>, outputUv : vec2<f32>, sourceUv : vec2<f32>, reactiveFactor : f32, depthClip : f32, edgeFactor : f32) -> UpsampleResult {
    let stability = clamp(uniforms.stabilityFactor, 0.0, 1.0);
    let sourceViewportF = viewport_f(uniforms.sourceViewport);
    let sourcePos = sourceUv * sourceViewportF - vec2<f32>(0.5);
    let basePixel = vec2<i32>(floor(sourcePos));
    let outputSourcePos = outputUv * sourceViewportF;
    let downscaleFactor = sourceViewportF / viewport_f(uniforms.outputViewport);
    let maxKernelWeight = min(1.99, 1.0 + (1.0 / max(downscaleFactor.x, 1e-4) - 1.0));
    let kernelBiasMax = maxKernelWeight * (1.0 - reactiveFactor);
    let kernelBiasMin = max(1.0, (1.0 + kernelBiasMax) * 0.3);
    let kernelBias = mix(kernelBiasMax, kernelBiasMin, max(0.25 * depthClip, reactiveFactor)) * mix(1.0, 0.68, stability);
    let rectificationCurveBias = mix(-2.5, -1.35, stability);

    var colorSum = vec3<f32>(0.0);
    var weightSum = 0.0;
    var positiveColorSum = vec3<f32>(0.0);
    var positiveWeightSum = 0.0;
    var rectificationWeightSum = 0.0;
    var box : RectificationBox;

    for (var y = -1; y <= 2; y++) {
        for (var x = -1; x <= 2; x++) {
            let samplePixel = clamp_pixel(basePixel + vec2<i32>(x, y), uniforms.sourceViewport);
            let sampleYCoCg = sample_prepared(samplePixel);
            let sampleCenter = vec2<f32>(samplePixel) + vec2<f32>(0.5) - uniforms.currentJitter;
            let sampleOffset = sampleCenter - outputSourcePos;
            let sampleWeight = lanczos2_approx_sq(dot(sampleOffset * kernelBias, sampleOffset * kernelBias));
            colorSum += sampleYCoCg * sampleWeight;
            weightSum += sampleWeight;

            // A strictly positive kernel cannot ring across a high-contrast
            // silhouette. It is accumulated alongside Lanczos and only mixed
            // in for detected geometry edges below.
            let positiveWeight = exp(-1.35 * dot(sampleOffset, sampleOffset));
            positiveColorSum += sampleYCoCg * positiveWeight;
            positiveWeightSum += positiveWeight;

            let rectificationWeight = exp(rectificationCurveBias * dot(sampleOffset, sampleOffset));
            rectify_add_sample(x == -1 && y == -1, &box, sampleYCoCg, rectificationWeight);
            rectificationWeightSum += rectificationWeight;
        }
    }

    box.boxCenter /= max(rectificationWeightSum, 1e-5);
    box.boxVec = sqrt(max(box.boxVec / max(rectificationWeightSum, 1e-5) - box.boxCenter * box.boxCenter, vec3<f32>(0.0)));

    var result : UpsampleResult;
    result.weight = weightSum;
    let lanczosColor = colorSum / max(weightSum, 1e-5);
    let positiveColor = positiveColorSum / max(positiveWeightSum, 1e-5);
    result.colorYCoCg = mix(
        lanczosColor, positiveColor,
        edgeFactor * clamp(uniforms.edgeSoftness, 0.0, 1.0));
    box.boxVec *= mix(1.0, 1.75, stability) * max(uniforms.rectificationBoxScale, 0.25);
    result.colorYCoCg = clamp(result.colorYCoCg, box.aabbMin, box.aabbMax);
    result.box = box;
    return result;
}

fn update_luma_history(outputPixel : vec2<i32>, currentLuma : f32, historyUv : vec2<f32>, validHistory : bool) -> vec4<f32> {
    var history = vec4<f32>(0.0);
    if (validHistory) {
        history = sample_catmull_rom_color(historyLumaTexture, historyUv, uniforms.outputViewport);
    }
    return vec4<f32>(currentLuma, history.x, history.y, history.z);
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) globalInvocationId : vec3<u32>) {
    if (globalInvocationId.x >= uniforms.outputViewport.x || globalInvocationId.y >= uniforms.outputViewport.y) {
        return;
    }

    let outputPixel = vec2<i32>(globalInvocationId.xy);
    let exposureValue = frame_exposure(exposureSlots[EXPOSURE_SLOT_VALUE], uniforms.exposureEnabled);
    let outputUv = uv_from_pixel(globalInvocationId.xy, uniforms.outputViewport);
    let surface = find_surface(outputUv);
    let edgeFactor = surface_edge_factor(surface);
    let historyUv = outputUv - surface.motion;
    let validHistory = uniforms.historyValid != 0u && is_uv_inside(historyUv);
    let depthClip = textureLoad(depthClipTexture, pixel_from_uv(surface.sourceUv, uniforms.sourceViewport), 0).x;
    let reactiveMasks = textureLoad(reactiveMasksTexture, pixel_from_uv(surface.sourceUv, uniforms.sourceViewport), 0).xy;
    var thisFrameReactive = clamp(max(reactiveMasks.x, depthClip * 0.1), 0.0, 0.99);
    let upsampled = compute_upsampled(
        outputPixel, outputUv, surface.sourceUv, thisFrameReactive, depthClip,
        edgeFactor);
    let currentRgb = max(ycocg_to_rgb(upsampled.colorYCoCg), vec3<f32>(0.0));
    let currentLuma = perceived_luma(currentRgb);

    var finalRgb = currentRgb;
    var disocclusion = 1.0;
    var temporalReactive = 1.0;
    var lockStatus = textureLoad(lockStatusScratchTexture, outputPixel, 0).xy;
    var debugTermsA = vec4<f32>(0.0);
    var debugTermsB = vec4<f32>(0.0);
    // A pixel with no history starts its ramp at exactly this frame's evidence,
    // so the frame after a reset blends one-to-one rather than jumping straight
    // to the configured rate.
    var accumulatedWeight = max(upsampled.weight, 1.0e-3);

    if (validHistory) {
        let stochasticInput = uniforms.stochasticInput != 0u;
        let motionPixelsVector = abs(
            surface.motion * viewport_f(uniforms.outputViewport));
        let jitterPixelsVector = abs(
            (uniforms.currentJitter - uniforms.previousJitter) /
            viewport_f(uniforms.sourceViewport) *
            viewport_f(uniforms.outputViewport));
        // Projection conventions disagree about the jitter sign, but its
        // magnitude is known exactly. Remove that envelope per axis only from
        // confidence decisions; the reprojected history UV remains untouched.
        let sceneMotionPixels = length(max(
            motionPixelsVector - jitterPixelsVector, vec2<f32>(0.0)));
        // Jitter-free motion is nearly zero on a static camera. Fade the
        // relaxation out before one output pixel of real motion so moving
        // silhouettes and newly revealed surfaces keep the normal rejection
        // path. This is temporal coverage accumulation, not generic ghosting.
        let stationary = 1.0 - smoothstep(0.05, 0.85, sceneMotionPixels);
        let stationaryEdge = edgeFactor * stationary *
            clamp(uniforms.stationaryEdgeBlend, 0.0, 1.0);
        let geometryRejectionScale = 1.0 - 0.97 * stationaryEdge;
        // Depth clip is a footprint disagreement, not proof of disocclusion,
        // on a stationary jittered image. Keep identity/normal corroboration,
        // but prevent this one scalar from vetoing history by itself. It
        // returns to full authority as soon as scene motion exceeds jitter.
        let stationaryDepthScale = 1.0 - 0.97 * stationary *
            clamp(uniforms.stationaryEdgeBlend, 0.0, 1.0);
        let effectiveDepthClip = depthClip *
            min(geometryRejectionScale, stationaryDepthScale);
        // History is stored linear - RCAS and the caller read that same texture -
        // so it is compressed on load and the result is expanded again at the
        // store. Alpha carries the reactive value and is not a colour.
        let historyColor = sample_catmull_rom_color(historyColorTexture, historyUv, uniforms.outputViewport);
        // Exposure is this frame's; the history was stored un-exposed precisely
        // so a change in exposure between frames does not corrupt it.
        let historyRgb = prepare_for_accumulation(
            historyColor.rgb, uniforms.tonemapAccumulation, exposureValue);
        let historyNormalSample = sample_catmull_rom_color(historyNormalTexture, historyUv, uniforms.outputViewport).xyz;
        let historyNormal = normalize(select(vec3<f32>(0.0, 0.0, 1.0), historyNormalSample, length(historyNormalSample) > 1e-5));
        let normalConfidence = smoothstep(uniforms.normalThreshold, 1.0, dot(surface.normal, historyNormal));
        var depthDisocclusion = 0.0;
        if (stochasticInput) {
            let currentIsBackground = is_background_depth(surface.depth, uniforms.reverseDepth != 0u);
            if (currentIsBackground) {
                // No surface means nothing valid to reproject; do not let a
                // background pixel decay toward stale history over several
                // frames just because both frames read as "background".
                depthDisocclusion = 1.0;
            } else {
                // Only a surface/background class change is a reliable
                // disocclusion signal here. Reverse-Z device depth moves
                // substantially under camera translation even when the motion
                // vector reprojects the same surface, so any depth-magnitude
                // test rejects valid history during ordinary movement (see
                // testTsrReprojectsStochasticLightingWithoutCurrentFrameClipping).
                // Detecting a surface revealed from behind another surface
                // needs geometry identity, not depth; the normal check below
                // is currently the only substitute.
                let historyDepth = textureLoad(
                    historyDepthTexture,
                    pixel_from_uv(historyUv, uniforms.outputViewport),
                    0).x;
                depthDisocclusion = select(
                    0.0,
                    1.0,
                    currentIsBackground != is_background_depth(historyDepth, uniforms.reverseDepth != 0u));
            }
        } else {
            let historyDepth = sample_catmull_rom_scalar(historyDepthTexture, historyUv, uniforms.outputViewport);
            depthDisocclusion = smoothstep(uniforms.depthThreshold * 0.5, uniforms.depthThreshold, abs(surface.depth - historyDepth));
        }
        depthDisocclusion *= geometryRejectionScale;
        let identityMismatch = identity_disocclusion(historyUv, surface.identity);
        // Identity lowers confidence; it does not veto agreeing evidence.
        //
        // Combining all three signals with a plain `max` lets any one of them
        // throw the history away on its own, and identity is the one that
        // cannot be trusted to do that. A correct one-pixel jitter makes a
        // discrete voxel resolve flip identity on every boundary pixel every
        // frame, so identity vetoed perfectly agreeing depth and normal and the
        // accumulator discarded its history continuously - measurably so: the
        // output's frame-to-frame instability scaled *linearly* with jitter
        // amplitude, which is what "no accumulation at all" looks like, and the
        // screen read as shaking rather than resolving.
        //
        // Depth and normal corroborate each other and are safe to veto with.
        // Identity on its own only takes confidence part of the way down, so a
        // flip across a voxel boundary blends rather than resets; corroborated
        // by either of the others it still rejects fully, which is what keeps
        // genuine disocclusions from ghosting.
        // A matching identity is positive evidence, and until now it was only
        // ever used as negative evidence.
        //
        // The normal veto has the same defect the identity veto had, for the
        // same reason. Voxel faces are axis aligned, so the two faces meeting
        // at a silhouette are perpendicular: a half-pixel jitter that moves the
        // resolve from one face of a voxel to the other takes
        // `dot(normal, historyNormal)` to zero and `normalConfidence` with it,
        // and the pixel throws its history away. That is not a disocclusion -
        // it is the same voxel, sampled at a different sub-pixel position.
        //
        // Identity is what separates the two cases, and it is already computed.
        // When the surface under this pixel is present in the history footprint
        // the history does belong to it whatever the face normal says, so the
        // normal is demoted to a confidence term. When identity disagrees too,
        // or identity is not available, the normal still vetoes at full
        // strength and genuine disocclusions still reject.
        let identityConfirms =
            select(0.0, 1.0 - identityMismatch, uniforms.identityRejection != 0u);
        let normalRejection = (1.0 - normalConfidence) *
            mix(1.0, clamp(uniforms.normalRejectionStrength, 0.0, 1.0), identityConfirms) *
            geometryRejectionScale;
        let corroborated = max(depthDisocclusion, normalRejection);
        let identityAlone =
            clamp(uniforms.identityRejectionStrength, 0.0, 1.0) *
            geometryRejectionScale;
        let identityTerm =
            identityMismatch * mix(identityAlone, 1.0, corroborated);
        disocclusion = clamp(max(corroborated, identityTerm), 0.0, 1.0);

        let historyYCoCg = rgb_to_ycocg(historyRgb);
        let stability = clamp(uniforms.stabilityFactor, 0.0, 1.0);
        let rectificationMargin = vec3<f32>(0.015 * stability);
        let clippedHistoryYCoCg = clip_aabb(
            historyYCoCg,
            upsampled.colorYCoCg,
            upsampled.box.boxCenter - upsampled.box.boxVec - rectificationMargin,
            upsampled.box.boxCenter + upsampled.box.boxVec + rectificationMargin);
        let clippedHistoryRgb = max(ycocg_to_rgb(clippedHistoryYCoCg), vec3<f32>(0.0));
        // A dark Monte Carlo sample is not evidence that a bright historical
        // sample was invalid. Preserve stochastic history and let geometry
        // validation determine whether it may be reused.
        let resolvedHistoryRgb = select(clippedHistoryRgb, historyRgb, stochasticInput);
        let luminanceDiff = select(abs(luma(currentRgb) - luma(resolvedHistoryRgb)), 0.0, stochasticInput);
        var lockContribution = 0.0;
        if (!stochasticInput) {
            let lockLumaDiff = 1.0 - min_divided_by_max(lockStatus.y, currentLuma);
            let newLock = textureLoad(newLocksTexture, outputPixel, 0).x;
            if (newLock > 0.5) {
                lockStatus.x = select(1.0, 2.0, lockStatus.x > 0.0);
                lockStatus.y = currentLuma;
            }
            thisFrameReactive = max(thisFrameReactive, clamp((lockLumaDiff - 0.1) * 10.0, 0.0, 1.0));
            lockStatus.x *= 1.0 - thisFrameReactive;
            lockStatus.x *= 1.0 - reactiveMasks.y;
            lockStatus.x *= select(0.0, 1.0, depthClip < 0.1);
            lockContribution = clamp(clamp(lockStatus.x - 1.0, 0.0, 1.0) * 4.0 * min_divided_by_max(lockStatus.y, currentLuma), 0.0, 1.0);
        } else {
            lockStatus = vec2<f32>(0.0);
        }

        let baseAccumulation = clamp(1.0 - 1.0 / max(uniforms.accumulationFrames * mix(1.0, 1.75, stability), 1.0), 0.0, mix(0.98, 0.993, stability));
        // Each factor is named rather than folded into a running product so it
        // can be written out individually; see the debug targets above.
        //
        // A geometric disocclusion is a hard invalidation: the history at this
        // pixel belongs to a different surface, so keeping a fraction of it
        // is never correct and shows up as the occluder's lighting fading out
        // over the next few frames. Stability must not weaken this term; it
        // still governs the soft reactive and motion terms below.
        let disocclusionFactor = 1.0 - clamp(
            max(disocclusion, effectiveDepthClip) *
                uniforms.disocclusionScale,
            0.0, 1.0);
        let reactiveFactor = 1.0 - clamp(thisFrameReactive * mix(0.65, 0.32, stability), 0.0, 1.0);
        var motionFactor = 1.0;
        if (!stochasticInput) {
            motionFactor = 1.0 - clamp(sceneMotionPixels * uniforms.motionScale * mix(1.0, 0.45, stability), 0.0, 1.0);
        }
        let rejectionRetention =
            clamp(disocclusionFactor * reactiveFactor * motionFactor, 0.0, 1.0);
        let historyWeightPreLock = baseAccumulation * rejectionRetention;
        var historyWeight = mix(historyWeightPreLock, max(historyWeightPreLock, mix(0.92, 0.965, stability)), lockContribution);

        // Per-pixel accumulated kernel weight, which is what decides the blend
        // when `accumulationWeighting` is on.
        //
        // `compute_upsampled` already produces `upsampled.weight` - the summed
        // Lanczos weight this output pixel's centre received from the source
        // grid this frame - and until now it was computed and discarded, with
        // the blend driven entirely by `baseAccumulation`, a constant of the
        // uniforms. One screen-wide rate cannot express the thing that actually
        // varies at a 0.5 render scale: how much of this output pixel was
        // measured this frame rather than interpolated.
        //
        // The consequence that matters most is not the steady state - it is the
        // ramp. A constant rate has none. A pixel that was disoccluded last
        // frame has exactly one frame of evidence, and the frame after that it
        // is handed 0.966 retention, locking in a single sample as though it
        // were thirty frames of history. Dividing by the accumulated weight
        // gives the 1/n ramp instead: full current frame, then a half, a third,
        // converging to the configured rate once the pixel has earned it.
        //
        // Rejection multiplies the accumulated *evidence*, not the colour, so a
        // disocclusion restarts the ramp rather than resetting the colour and
        // then immediately over-trusting it again.
        if (uniforms.accumulationWeighting != 0u) {
            // Bilinear, not catmull-rom: this is a smooth scalar being
            // reprojected, it gains nothing from the extra taps, and the
            // negative lobes could hand back a negative weight.
            let priorWeight = max(sample_bilinear_scalar(
                historyAccumulationTexture, historyUv, uniforms.outputViewport), 0.0) *
                rejectionRetention;
            // Floored so a pixel the kernel gave nothing to still advances its
            // ramp rather than dividing by zero and freezing forever.
            let sampleWeight = max(upsampled.weight, 1.0e-3);
            // Capped in the same units `accumulationFrames` already means, so
            // the dial keeps its meaning: at the cap the ratio below reduces to
            // 1 - 1/(N+1), which is `baseAccumulation`.
            let maxAccumulatedWeight =
                max(uniforms.accumulationFrames * mix(1.0, 1.75, stability), 1.0);
            accumulatedWeight = min(priorWeight + sampleWeight, maxAccumulatedWeight);
            let weighted = priorWeight / max(priorWeight + sampleWeight, 1.0e-5);
            // The lock keeps the same authority it had over the constant rate.
            historyWeight = mix(
                weighted, max(weighted, mix(0.92, 0.965, stability)), lockContribution);
        }
        // The screen mean of `historyWeight` cannot tell "every pixel keeps a
        // little less" from "a few pixels keep nothing", and those want
        // completely different fixes. This counts the second directly.
        let rejected = historyWeight < 0.5;
        let rejectedFraction = select(0.0, 1.0, rejected);
        // Attribution *within* that minority. Suspects were being tested one at
        // a time by turning each off and watching the screen mean, and that was
        // wrong twice: the rejecting pixels are 0.2% of the frame, so any
        // whole-frame average is dominated by the 99.8% that do not reject and
        // is nearly blind to them. These count causes among rejecting pixels
        // only, in one run, so the shares can be compared directly.
        let depthClipCause = select(
            0.0, 1.0, rejected && effectiveDepthClip > 0.4);
        let normalCause = select(0.0, 1.0, rejected && normalRejection > 0.4);
        let identityCause = select(0.0, 1.0, rejected && identityTerm > 0.4);
        debugTermsA = vec4<f32>(depthClipCause, disocclusionFactor, rejectedFraction, motionFactor);
        debugTermsB = vec4<f32>(normalCause, identityCause, historyWeight, depthClip);

        finalRgb = mix(currentRgb, resolvedHistoryRgb, clamp(historyWeight, 0.0, mix(0.98, 0.993, stability)));
        temporalReactive = select(
            clamp(max(abs(historyColor.a), max(thisFrameReactive, luminanceDiff * uniforms.reactiveScale)), 0.0, 1.0),
            thisFrameReactive,
            stochasticInput);
        if (!stochasticInput && sceneMotionPixels >= 10.0) {
            temporalReactive = -max(temporalReactive, 1e-5);
        }
    }

    let lumaHistory = update_luma_history(outputPixel, currentLuma, historyUv, validHistory);
    // Back to linear for storage: this texture is what RCAS samples and what
    // the caller presents, and the next frame compresses it again on load.
    let storedRgb = resolve_from_accumulation(
        finalRgb, uniforms.tonemapAccumulation, exposureValue);
    textureStore(outputHistoryColorTexture, outputPixel, vec4<f32>(storedRgb, temporalReactive));
    // Only maintained when the weighted blend is on, so callers that leave it
    // off can bind a placeholder instead of an output-sized surface.
    if (uniforms.accumulationWeighting != 0u) {
        textureStore(
            outputAccumulationTexture, outputPixel,
            vec4<f32>(accumulatedWeight, 0.0, 0.0, 0.0));
    }
    textureStore(outputHistoryDepthTexture, outputPixel, vec4<f32>(surface.depth, 0.0, 0.0, 0.0));
    textureStore(outputHistoryNormalTexture, outputPixel, vec4<f32>(surface.normal, 1.0));
    // Only maintained when identity rejection is on, so callers that do not
    // use it can bind a placeholder instead of an output-sized surface.
    if (uniforms.identityRejection != 0u) {
        textureStore(outputHistoryIdTexture, outputPixel, vec4<u32>(surface.identity, 0u, 0u));
    }
    if (uniforms.debugTerms != 0u) {
        textureStore(outputDebugTermsATexture, outputPixel, debugTermsA);
        textureStore(outputDebugTermsBTexture, outputPixel, debugTermsB);
    }
    textureStore(outputLumaTexture, outputPixel, lumaHistory);
    textureStore(outputLockStatusTexture, outputPixel, vec4<f32>(lockStatus, 0.0, 0.0));
    textureStore(outputDisocclusionMaskTexture, outputPixel, vec4<f32>(clamp(disocclusion, 0.0, 1.0), 0.0, 0.0, 0.0));
}
