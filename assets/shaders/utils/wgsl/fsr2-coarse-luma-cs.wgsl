#include "fsr2-temporal-common.wgsl"

// Coarse luminance grid, one cell per COARSE_LUMA_TILE square of source pixels.
//
// This is the input to shading-change detection, and it is deliberately not a
// per-pixel quantity. FSR 3.1 reads a coarse mip of its luminance pyramid for the
// same reason: at this scale sub-pixel jitter and aliasing average out, so a
// frame-to-frame difference that survives is a change in the *lighting* rather
// than in where the samples landed. Every per-pixel version of this question that
// has been tried in this pipeline - identity, the normal veto, the luma ring
// against pre-accumulation luma - reported jitter as change and cost the image its
// history everywhere.
//
// A tile reduction rather than a real SPD mip chain. It occupies the same place in
// the algorithm and needs no per-mip storage views, but it is one fixed scale
// where a pyramid would offer several, and it is not bilinearly consistent with
// the finer levels because there are none.
@group(0) @binding(0) var<uniform> uniforms : Fsr2Uniforms;
@group(0) @binding(1) var currentColorTexture : texture_2d<f32>;
@group(0) @binding(2) var outputCoarseLumaTexture : texture_storage_2d<r32float, write>;

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) globalInvocationId : vec3<u32>) {
    let extent = coarse_luma_extent(uniforms.sourceViewport);
    if (globalInvocationId.x >= extent.x || globalInvocationId.y >= extent.y) {
        return;
    }

    let origin = globalInvocationId.xy * COARSE_LUMA_TILE;
    var logSum = 0.0;
    var counted = 0u;
    for (var y = 0u; y < COARSE_LUMA_TILE; y++) {
        for (var x = 0u; x < COARSE_LUMA_TILE; x++) {
            let sample = origin + vec2<u32>(x, y);
            if (sample.x >= uniforms.sourceViewport.x ||
                sample.y >= uniforms.sourceViewport.y) {
                continue;
            }
            let luminance = luma(max(
                textureLoad(currentColorTexture, vec2<i32>(sample), 0).rgb,
                vec3<f32>(0.0)));
            // A geometric mean, so the cell tracks its own middle rather than being
            // dragged around by its brightest pixel - a single specular highlight
            // entering a cell is not a shading change across it.
            //
            // Every in-bounds pixel counts, with the floor applied by clamping
            // rather than by exclusion. Excluding dim pixels averages over a
            // *varying population*: a cell straddling a lit/unlit boundary changes
            // which pixels qualify from frame to frame, and its mean then swings by
            // many stops for no change in the lighting at all. That defect made the
            // shading-change threshold irrelevant - the firing cells saturated it,
            // so raising the threshold fourfold changed the measured cost not at all.
            logSum += clamp(
                log(max(luminance, EXPOSURE_LUMINANCE_FLOOR)), -15.0, 15.0);
            counted += 1u;
        }
    }

    let value = select(
        COARSE_LUMA_SENTINEL, logSum / f32(max(counted, 1u)), counted > 0u);
    textureStore(
        outputCoarseLumaTexture, vec2<i32>(globalInvocationId.xy),
        vec4<f32>(value, 0.0, 0.0, 0.0));
}
