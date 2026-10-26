struct Fsr2Uniforms {
    sourceViewport : vec2<u32>,
    outputViewport : vec2<u32>,
    currentJitter : vec2<f32>,
    previousJitter : vec2<f32>,
    historyValid : u32,
    reverseDepth : u32,
    frameIndex : u32,
    stochasticInput : u32,
    accumulationFrames : f32,
    depthThreshold : f32,
    normalThreshold : f32,
    motionScale : f32,
    reactiveScale : f32,
    disocclusionScale : f32,
    sharpenStrength : f32,
    inputDisocclusionScale : f32,
    stabilityFactor : f32,
    identityRejection : u32,
    identityRejectionStrength : f32,
    // When set, the accumulator writes its history-weight term chain to the
    // two debug targets so each factor can be reduced to a screen mean. Off in
    // production: the targets are 1x1 placeholders and the stores are skipped.
    debugTerms : u32,
    // Whether the supplied motion vectors carry the frame-to-frame projection
    // jitter. See the removal in `fsr2-reconstruct-dilate-cs.wgsl`.
    motionIncludesJitter : u32,
    // Rejection a normal mismatch causes when geometry identity says the
    // surface is still there; see the accumulator. One is the old behaviour,
    // where the normal vetoed on its own.
    normalRejectionStrength : f32,
    // How hard the FSR 3.1 accumulator reacts to a pixel whose luminance has
    // been oscillating across the luma ring. Ignored by the FSR2 accumulator,
    // which does not read the ring at all.
    lumaInstabilityStrength : f32,
    // Run reconstruction and accumulation in a reversibly tonemapped space
    // rather than on raw linear HDR. See `tonemap_for_accumulation`.
    tonemapAccumulation : u32,
    // Drive the history blend from a per-pixel accumulated kernel weight
    // instead of one screen-wide rate. See the accumulators.
    accumulationWeighting : u32,
    // Sharpen with real RCAS instead of the unsharp mask this pass shipped with.
    robustSharpen : u32,
    // Projection coefficients for turning device depth into a positive view
    // distance: `distance = depthLinearB / (deviceDepth + depthLinearA)`, with
    // A and B taken straight from the projection matrix's (2,2) and (2,3). Exact
    // for reverse-Z, normal Z and an infinite far plane alike, which is why the
    // matrix entries are passed rather than near and far - deriving them here
    // would have to assume a convention.
    depthLinearA : f32,
    depthLinearB : f32,
    // Relative view-distance separation at which a pixel behind the nearest
    // surface reprojecting onto its history counts as fully disoccluded. A
    // fraction rather than an absolute distance so it is scene-scale free.
    depthSeparationScale : f32,
    // Use the reconstructed previous-frame depth for the disocclusion signal
    // instead of differencing this frame's depth against the history depth.
    reconstructedDepth : u32,
    // Apply auto-exposure before range compression. Ignored unless
    // tonemapAccumulation is on - exposure without compression only rescales the
    // image and would change nothing but its brightness.
    exposureEnabled : u32,
    // Extra width on the neighbourhood rectification box.
    //
    // The box is a standard deviation over the source neighbourhood, and how much
    // it actually constrains anything depends entirely on the space it is measured
    // in. On unexposed linear HDR it is set almost wholly by the brightest tap and
    // is effectively inert; range compression makes it tight and meaningful, which
    // is why compression *softens* the image - a tight box clips exactly the
    // accumulated sub-pixel detail anti-aliasing is made of. This is the dial that
    // lets the box be re-fitted for the space it is now measured in, instead of
    // inheriting a width tuned where it did nothing.
    rectificationBoxScale : f32,
    // How hard a coarse-scale luminance change costs a pixel its history. See
    // `shading_change` and the coarse-luma stage. Zero disables it.
    shadingChangeStrength : f32,
    // Stops of coarse luminance change below which nothing is charged. A full
    // change is two stops above it. Exposed rather than fixed because it is the
    // dial that separates "the lighting changed" from "the lighting estimate is
    // noisy", and where that line sits is a property of the renderer feeding it.
    shadingChangeStops : f32,
    // Suppress jitter-driven history resets only on stationary geometry edges.
    // This is deliberately separate from the general rejection strengths: a
    // moving edge must still disocclude normally.
    stationaryEdgeBlend : f32,
    // Positive-kernel reconstruction mixed into Lanczos only at geometry
    // boundaries. It trades a small, local softness for less sub-pixel ringing.
    edgeSoftness : f32,
}

struct RectificationBox {
    aabbMin : vec3<f32>,
    aabbMax : vec3<f32>,
    boxCenter : vec3<f32>,
    boxVec : vec3<f32>,
}

struct UpsampleResult {
    colorYCoCg : vec3<f32>,
    weight : f32,
    box : RectificationBox,
}

fn viewport_f(viewport : vec2<u32>) -> vec2<f32> {
    return vec2<f32>(f32(viewport.x), f32(viewport.y));
}

fn uv_from_pixel(pixelCoords : vec2<u32>, viewport : vec2<u32>) -> vec2<f32> {
    return (vec2<f32>(pixelCoords) + vec2<f32>(0.5)) / viewport_f(viewport);
}

fn clamp_pixel(pixel : vec2<i32>, viewport : vec2<u32>) -> vec2<i32> {
    return clamp(pixel, vec2<i32>(0, 0), vec2<i32>(i32(viewport.x) - 1, i32(viewport.y) - 1));
}

fn pixel_from_uv(uv : vec2<f32>, viewport : vec2<u32>) -> vec2<i32> {
    let viewportSize = viewport_f(viewport);
    let pixel = clamp(uv * viewportSize, vec2<f32>(0.0), viewportSize - vec2<f32>(1.0));
    return vec2<i32>(pixel);
}

fn source_uv_from_output_uv(outputUv : vec2<f32>, jitter : vec2<f32>, sourceViewport : vec2<u32>) -> vec2<f32> {
    return clamp(outputUv + jitter / viewport_f(sourceViewport), vec2<f32>(0.0), vec2<f32>(1.0));
}

fn is_uv_inside(uv : vec2<f32>) -> bool {
    return uv.x >= 0.0 && uv.y >= 0.0 && uv.x <= 1.0 && uv.y <= 1.0;
}

fn luma(color : vec3<f32>) -> f32 {
    return dot(color, vec3<f32>(0.299, 0.587, 0.114));
}

fn perceived_luma(color : vec3<f32>) -> f32 {
    return pow(max(luma(max(color, vec3<f32>(0.0))), 0.0), 1.0 / 6.0);
}

// Source pixels per side of one coarse-luminance cell.
//
// This is the scale that makes shading-change detection work at all, and the reason
// it is coarse rather than per pixel: at 16x16 the sub-pixel jitter and the aliasing
// average out, so a difference that survives is a difference in *shading* rather
// than in where the samples landed. Measured per pixel it would report every
// jittered edge in the frame as a shading change - the same trap the identity test,
// the normal veto and the luma ring each fell into first.
const COARSE_LUMA_TILE : u32 = 16u;
// Written into a cell no lit pixel contributed to. Far below any log luminance the
// reduction can produce for a lit surface.
const COARSE_LUMA_SENTINEL : f32 = -1.0e4;
const COARSE_LUMA_INV_LOG2 : f32 = 1.4426950408889634;
// Stops of coarse luminance change that count as nothing / as a full change.
//
// This has to sit above the frame-to-frame variation the *lighting estimate*
// produces on its own, which is a different thing from the lighting changing.
// This renderer's probe lighting is stochastic and updates on a schedule, so a
// cell's mean luminance moves every frame by design; a threshold that catches
// that reads noise as a shading change, drops the history that was suppressing
// the noise, and exposes more of it - a feedback loop, and measurably so.
const COARSE_LUMA_STOPS_RANGE : f32 = 2.0;

fn coarse_luma_extent(viewport : vec2<u32>) -> vec2<u32> {
    return max(
        (viewport + vec2<u32>(COARSE_LUMA_TILE - 1u)) / COARSE_LUMA_TILE,
        vec2<u32>(1u));
}

// Auto-exposure accumulator layout, shared by the stage that fills it and every
// stage that reads the result out of it.
const EXPOSURE_SLOT_SUM : u32 = 0u;
const EXPOSURE_SLOT_COUNT : u32 = 1u;
const EXPOSURE_SLOT_VALUE : u32 = 2u;
const EXPOSURE_SLOTS : u32 = 4u;
const EXPOSURE_FIXED_SCALE : f32 = 256.0;
// Below this a pixel contributes nothing to the geometric mean; see the
// reduction. Small enough to keep dim lit surfaces in, large enough to exclude
// the background.
const EXPOSURE_LUMINANCE_FLOOR : f32 = 1.0e-4;
const EXPOSURE_MINIMUM : f32 = 1.0e-3;
const EXPOSURE_MAXIMUM : f32 = 1.0e3;

// Exposure this frame, or one when the caller has not enabled it.
//
// Read from a buffer rather than the uniforms because it is computed on the GPU
// from this frame's own image, inside the same pass. Getting it to the CPU would
// cost a readback and a frame of latency, and an exposure that lags the image it
// describes is worse than none.
//
// Takes the raw stored word rather than the buffer: WGSL core does not allow a
// `ptr<storage, ...>` function parameter, so each stage indexes its own binding
// and passes the value in.
fn frame_exposure(storedWord : i32, enabled : u32) -> f32 {
    if (enabled == 0u) {
        return 1.0;
    }
    let stored = bitcast<f32>(storedWord);
    // A zero here means the resolve has not run, which must read as "no
    // exposure" rather than as a multiplier that erases the image.
    if (!(stored > 0.0)) {
        return 1.0;
    }
    return stored;
}

// Coarse-scale luminance change between this frame and the last, at one point.
//
// Both arguments are log luminances, so their difference is a ratio and the figure
// is brightness-independent: a doubling reads the same in shadow as in a highlight,
// which a linear difference does not. The threshold is therefore in stops.
//
// This is the one signal in the pipeline that can catch a *shading* change no motion
// vector can explain, which is directly this renderer's case - probe lighting moves
// while geometry does not, and every geometric validation term correctly reports
// that nothing happened.
fn shading_change(
    currentLogLuma : f32, previousLogLuma : f32, strength : f32,
    threshold : f32) -> f32 {
    if (strength <= 0.0) {
        return 0.0;
    }
    // A cell that has never resolved carries the sentinel and is not evidence of
    // anything; treating "no data" as "changed" would deny history to exactly the
    // pixels that have just started accumulating it.
    if (currentLogLuma <= COARSE_LUMA_SENTINEL ||
        previousLogLuma <= COARSE_LUMA_SENTINEL) {
        return 0.0;
    }
    let stops = abs(currentLogLuma - previousLogLuma) * COARSE_LUMA_INV_LOG2;
    let floorStops = max(threshold, 0.0);
    return clamp(
        smoothstep(floorStops, floorStops + COARSE_LUMA_STOPS_RANGE, stops) *
            strength,
        0.0, 1.0);
}

// Reversible range compression for the reconstruction and accumulation stages.
//
// Everything between the dilate stage and the final store is a *resampling*
// problem - a Lanczos kernel with negative lobes, a neighbourhood variance box,
// and an AABB clip of the reprojected history - and all three are built on the
// assumption that a neighbourhood's values are commensurable. On raw linear HDR
// they are not. This renderer has no tonemap and no exposure anywhere in the
// chain, and its finite lights ship at intensity 12, so a lit edge routinely
// puts a value of 10+ next to a value of 0.1:
//
//   - Lanczos2's negative lobes overshoot in proportion to the bright tap, so a
//     high-contrast edge rings. `max(rgb, 0)` hides the dark half of the ring
//     and leaves the bright fringe.
//   - `box.boxVec` is a standard deviation in linear units, so it is set almost
//     entirely by the brightest tap in the footprint. The history clip is then
//     far too loose around highlights and far too tight in shadow - the one
//     place it is meant to be doing work.
//   - A fixed rectification margin (0.015) means something completely different
//     at 0.1 than at 10.
//
// Compressing to [0,1) first makes the neighbourhood commensurable, which is
// the property all three stages actually need. The transform is exact in both
// directions, so nothing is lost: only the space the arithmetic happens in
// changes.
fn tonemap_for_accumulation(rgb : vec3<f32>) -> vec3<f32> {
    let color = max(rgb, vec3<f32>(0.0));
    return color / (1.0 + max(max(color.r, color.g), color.b));
}

// Exact inverse of the above. `peak` after compression is p/(1+p), so
// 1 - peak is 1/(1+p) and dividing by it restores the original scale.
//
// The epsilon caps the reconstructible value rather than guarding a division
// that can otherwise produce an infinity: a compressed peak of exactly 1 is a
// linear value of infinity, which no history should ever hold, and clamping it
// to 1e4 keeps one bad pixel from poisoning every neighbourhood it lands in for
// the rest of the run.
fn inverse_tonemap_for_accumulation(rgb : vec3<f32>) -> vec3<f32> {
    let color = max(rgb, vec3<f32>(0.0));
    let peak = max(max(color.r, color.g), color.b);
    return color / max(1.0 - peak, 1.0e-4);
}

// Both directions become the identity when the caller has not opted in, so the
// stages read the same either way and the flag is a single switch per stage.
//
// Exposure is folded in here because it has to be applied *before* compression to
// do anything: compressing first and scaling afterwards just rescales an already
// saturated value. The history textures are stored un-exposed, so a frame whose
// exposure differs from the one that wrote the history still reads it correctly -
// which is why exposure is a parameter of these two functions rather than
// something baked into the stored image.
// Exposure is deliberately gated on compression rather than applied on its own.
// Scaling a linear image by a constant changes nothing a later stage can see
// except where a *fixed* threshold is compared against it - the rectification
// margin and the reactive luminance difference - so applying it without
// compression only perturbs two tuned constants for no benefit.
fn prepare_for_accumulation(
    rgb : vec3<f32>, enabled : u32, exposureValue : f32) -> vec3<f32> {
    let color = max(rgb, vec3<f32>(0.0));
    if (enabled == 0u) {
        return color;
    }
    return tonemap_for_accumulation(color * exposureValue);
}

fn resolve_from_accumulation(
    rgb : vec3<f32>, enabled : u32, exposureValue : f32) -> vec3<f32> {
    let color = max(rgb, vec3<f32>(0.0));
    if (enabled == 0u) {
        return color;
    }
    return inverse_tonemap_for_accumulation(color) / max(exposureValue, 1.0e-6);
}

fn rgb_to_ycocg(color : vec3<f32>) -> vec3<f32> {
    let co = color.r - color.b;
    let tmp = color.b + 0.5 * co;
    let cg = color.g - tmp;
    let y = tmp + 0.5 * cg;
    return vec3<f32>(y, co, cg);
}

fn ycocg_to_rgb(color : vec3<f32>) -> vec3<f32> {
    let tmp = color.x - 0.5 * color.z;
    let g = color.z + tmp;
    let b = tmp - 0.5 * color.y;
    let r = b + color.y;
    return vec3<f32>(r, g, b);
}

fn catmull_rom_weights(t : f32) -> vec4<f32> {
    let t2 = t * t;
    let t3 = t2 * t;
    return vec4<f32>(
        -0.5 * t + t2 - 0.5 * t3,
        1.0 - 2.5 * t2 + 1.5 * t3,
        0.5 * t + 2.0 * t2 - 1.5 * t3,
        -0.5 * t2 + 0.5 * t3
    );
}

fn sample_catmull_rom_color(textureInput : texture_2d<f32>, uv : vec2<f32>, viewport : vec2<u32>) -> vec4<f32> {
    let viewportSize = viewport_f(viewport);
    let samplePos = uv * viewportSize - vec2<f32>(0.5);
    let basePixel = floor(samplePos);
    let frac = samplePos - basePixel;
    let weightsX = catmull_rom_weights(frac.x);
    let weightsY = catmull_rom_weights(frac.y);

    var result = vec4<f32>(0.0);
    for (var y = 0; y < 4; y++) {
        for (var x = 0; x < 4; x++) {
            let pixel = clamp_pixel(vec2<i32>(basePixel) + vec2<i32>(x - 1, y - 1), viewport);
            result += textureLoad(textureInput, pixel, 0) * (weightsX[x] * weightsY[y]);
        }
    }
    return result;
}

fn sample_catmull_rom_scalar(textureInput : texture_2d<f32>, uv : vec2<f32>, viewport : vec2<u32>) -> f32 {
    return sample_catmull_rom_color(textureInput, uv, viewport).x;
}

// Four-tap bilinear. Catmull-rom above is a sixteen-tap loop, which is the
// right trade when its result is reconstructed image data. It is the wrong
// trade for comparisons - relative-magnitude tests over a neighbourhood do not
// benefit from its sharpening, and paying 4x the fetches per neighbour makes
// such a loop dominate a whole pass.
fn sample_bilinear_scalar(textureInput : texture_2d<f32>, uv : vec2<f32>, viewport : vec2<u32>) -> f32 {
    let samplePos = uv * viewport_f(viewport) - vec2<f32>(0.5);
    let basePixel = floor(samplePos);
    let frac = samplePos - basePixel;
    let base = vec2<i32>(basePixel);
    let s00 = textureLoad(textureInput, clamp_pixel(base, viewport), 0).x;
    let s10 = textureLoad(textureInput, clamp_pixel(base + vec2<i32>(1, 0), viewport), 0).x;
    let s01 = textureLoad(textureInput, clamp_pixel(base + vec2<i32>(0, 1), viewport), 0).x;
    let s11 = textureLoad(textureInput, clamp_pixel(base + vec2<i32>(1, 1), viewport), 0).x;
    return mix(mix(s00, s10, frac.x), mix(s01, s11, frac.x), frac.y);
}

// Where a source pixel's surface lands in the previous frame, in the pixel index
// space the reconstructed-depth buffer is addressed by.
//
// Both the scatter that fills that buffer and the read that consumes it must
// derive this identically, which is why it lives here rather than in either
// stage. Getting it wrong does not fail loudly - it reports disocclusions on a
// static camera, because a read landing one slot over from the write picks up a
// neighbouring surface, and on a silhouette that neighbour is much nearer. That
// is exactly what a genuine disocclusion looks like to the test.
//
// Two corrections, both load-bearing. Source pixel centres sit on the *jittered*
// grid while the buffer is indexed on the stable unjittered one, so the jitter
// comes out before the velocity is applied. And the half-texel shift is applied
// here, once, so neither side can apply it twice or not at all.
fn reconstructed_depth_position(
    pixel : vec2<i32>,
    motion : vec2<f32>,
    jitter : vec2<f32>,
    sourceViewport : vec2<u32>) -> vec2<f32> {
    let sourceSize = viewport_f(sourceViewport);
    let displayUv = (vec2<f32>(pixel) + vec2<f32>(0.5) - jitter) / sourceSize;
    return (displayUv - motion) * sourceSize - vec2<f32>(0.5);
}

// "Nothing reprojected onto this slot", at the far end of whichever depth
// convention is in use.
fn reconstructed_depth_sentinel(reverseDepth : bool) -> u32 {
    return select(0xffffffffu, 0u, reverseDepth);
}

fn reconstructed_depth_index(slot : vec2<i32>, sourceViewport : vec2<u32>) -> u32 {
    return u32(slot.y) * sourceViewport.x + u32(slot.x);
}

fn reconstructed_depth_in_bounds(slot : vec2<i32>, sourceViewport : vec2<u32>) -> bool {
    return slot.x >= 0 && slot.y >= 0 &&
        slot.x < i32(sourceViewport.x) && slot.y < i32(sourceViewport.y);
}

fn lanczos2_approx_sq(x2Input : f32) -> f32 {
    let x2 = min(x2Input, 4.0);
    let a = (2.0 / 5.0) * x2 - 1.0;
    let b = (1.0 / 4.0) * x2 - 1.0;
    return ((25.0 / 16.0) * a * a - (25.0 / 16.0 - 1.0)) * (b * b);
}

fn clip_aabb(history : vec3<f32>, center : vec3<f32>, boxMin : vec3<f32>, boxMax : vec3<f32>) -> vec3<f32> {
    let direction = history - center;
    var t = 1.0;
    if (direction.x > 1e-5) {
        t = min(t, (boxMax.x - center.x) / direction.x);
    } else if (direction.x < -1e-5) {
        t = min(t, (boxMin.x - center.x) / direction.x);
    }
    if (direction.y > 1e-5) {
        t = min(t, (boxMax.y - center.y) / direction.y);
    } else if (direction.y < -1e-5) {
        t = min(t, (boxMin.y - center.y) / direction.y);
    }
    if (direction.z > 1e-5) {
        t = min(t, (boxMax.z - center.z) / direction.z);
    } else if (direction.z < -1e-5) {
        t = min(t, (boxMin.z - center.z) / direction.z);
    }
    return center + direction * clamp(t, 0.0, 1.0);
}

fn is_closer_depth(candidate : f32, current : f32, reverseDepth : bool) -> bool {
    return select(candidate < current, candidate > current, reverseDepth);
}

fn is_background_depth(depth : f32, reverseDepth : bool) -> bool {
    return select(depth >= 1.0 - 1e-6, depth <= 1e-6, reverseDepth);
}

fn min_divided_by_max(a : f32, b : f32) -> f32 {
    return min(a, b) / max(max(a, b), 1e-5);
}
