#include "fsr2-temporal-common.wgsl"

@group(0) @binding(0) var<uniform> uniforms : Fsr2Uniforms;
@group(0) @binding(1) var currentColorTexture : texture_2d<f32>;
@group(0) @binding(2) var sceneDepthTexture : texture_depth_2d;
@group(0) @binding(3) var motionVectorTexture : texture_2d<f32>;
@group(0) @binding(4) var preparedColorTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(5) var dilatedDepthTexture : texture_storage_2d<r32float, write>;
@group(0) @binding(6) var dilatedMotionTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(7) var lockInputLumaTexture : texture_storage_2d<r32float, write>;
// Nearest current-frame depth that reprojects onto each *previous-frame* pixel.
// Written by scatter below, consumed by the depth-clip stage. A buffer rather
// than a texture because WGSL has no texture atomics.
@group(0) @binding(8) var<storage, read_write> reconstructedDepth : array<atomic<u32>>;
@group(0) @binding(9) var<storage, read> exposureSlots : array<i32>;

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) globalInvocationId : vec3<u32>) {
    if (globalInvocationId.x >= uniforms.sourceViewport.x || globalInvocationId.y >= uniforms.sourceViewport.y) {
        return;
    }

    let pixel = vec2<i32>(globalInvocationId.xy);
    // Range-compressed here, once, so that every later stage - the Lanczos
    // kernel, the rectification box, the history clip and the blend - works on
    // commensurable values. The accumulator inverts it at its final store, so
    // the pass still hands linear HDR to RCAS and to the caller. See
    // `tonemap_for_accumulation`.
    let exposureValue = frame_exposure(exposureSlots[EXPOSURE_SLOT_VALUE], uniforms.exposureEnabled);
    let color = prepare_for_accumulation(
        textureLoad(currentColorTexture, pixel, 0).rgb,
        uniforms.tonemapAccumulation, exposureValue);
    let preparedYCoCg = rgb_to_ycocg(color);

    var nearestPixel = pixel;
    var nearestDepth = textureLoad(sceneDepthTexture, pixel, 0);
    for (var y = -1; y <= 1; y++) {
        for (var x = -1; x <= 1; x++) {
            let samplePixel = clamp_pixel(pixel + vec2<i32>(x, y), uniforms.sourceViewport);
            let sampleDepth = textureLoad(sceneDepthTexture, samplePixel, 0);
            if (is_closer_depth(sampleDepth, nearestDepth, uniforms.reverseDepth != 0u)) {
                nearestDepth = sampleDepth;
                nearestPixel = samplePixel;
            }
        }
    }

    // Motion vectors are currentUv - previousUv. A jittered projection adds the
    // current-minus-previous pixel jitter to that velocity, so it is removed
    // once here before any TSR stage consumes or stores the motion.
    //
    // Whether it is present at all is the caller's to declare, and getting that
    // wrong is not a small error: subtracting a delta that was never there
    // *injects* a jitter-sized velocity into a stationary image. Measured on
    // the sparse voxel renderer at full jitter on a static camera, that was
    // 0.62 px/axis of motion that does not exist, costing a further 4% of the
    // history every frame through the accumulator's motion term.
    let rawMotion = textureLoad(motionVectorTexture, nearestPixel, 0).xy;
    var jitterMotion = vec2<f32>(0.0);
    if (uniforms.motionIncludesJitter != 0u) {
        jitterMotion = (uniforms.currentJitter - uniforms.previousJitter) /
            viewport_f(uniforms.sourceViewport);
    }
    let dilatedMotion = rawMotion - jitterMotion;
    let lockLuma = perceived_luma(color);

    textureStore(preparedColorTexture, pixel, vec4<f32>(preparedYCoCg, lockLuma));
    textureStore(dilatedDepthTexture, pixel, vec4<f32>(nearestDepth, 0.0, 0.0, 0.0));
    textureStore(dilatedMotionTexture, pixel, vec4<f32>(dilatedMotion, 0.0, 0.0));
    textureStore(lockInputLumaTexture, pixel, vec4<f32>(lockLuma, 0.0, 0.0, 0.0));

    if (uniforms.reconstructedDepth == 0u) {
        return;
    }

    // Project this pixel's depth back to where its surface was last frame and
    // keep, per previous-frame pixel, the nearest depth that lands there.
    //
    // This is the input the disocclusion test actually needs and the pipeline
    // never had. What the depth-clip stage has been doing instead is
    // differencing this frame's depth against the *history depth texture* - a
    // different resolution, holding whatever surface won that pixel last frame -
    // and the comments in that file already say a reverse-Z magnitude difference
    // is not a usable rejection signal under camera translation.
    //
    // The question a disocclusion test has to answer is not "did depth change
    // here", which camera motion alone answers yes to. It is "does another,
    // nearer surface also claim the history I am about to read". Scattering into
    // previous-frame space and keeping the minimum answers exactly that: if this
    // pixel's own depth is well behind what is stored at its history location,
    // something closer reprojects there too and the history belongs to that.
    // The jitter removal and half-texel shift live in the shared helper so the
    // read side cannot drift from this.
    let previousPosition = reconstructed_depth_position(
        pixel, dilatedMotion, uniforms.currentJitter, uniforms.sourceViewport);
    let base = vec2<i32>(floor(previousPosition));
    let frac = previousPosition - floor(previousPosition);
    // Scatter across the bilinear footprint the history read will sample from,
    // not just the nearest texel - a surface that only partially covers a
    // previous pixel still contaminates the filtered history read from it. Taps
    // whose weight is negligible are skipped so a surface does not claim a pixel
    // it barely touches.
    let weights = vec4<f32>(
        (1.0 - frac.x) * (1.0 - frac.y),
        frac.x * (1.0 - frac.y),
        (1.0 - frac.x) * frac.y,
        frac.x * frac.y);
    let key = bitcast<u32>(nearestDepth);
    for (var tap = 0; tap < 4; tap++) {
        if (weights[tap] <= 0.01) {
            continue;
        }
        let slot = base + vec2<i32>(tap & 1, tap >> 1);
        if (!reconstructed_depth_in_bounds(slot, uniforms.sourceViewport)) {
            continue;
        }
        let index = reconstructed_depth_index(slot, uniforms.sourceViewport);
        // Nearest wins, and which integer extreme that is depends on the
        // convention: reverse-Z puts the near plane at one.
        if (uniforms.reverseDepth != 0u) {
            atomicMax(&reconstructedDepth[index], key);
        } else {
            atomicMin(&reconstructedDepth[index], key);
        }
    }
}
