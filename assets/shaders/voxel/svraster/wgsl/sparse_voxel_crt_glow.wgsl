#include "sparse_voxel_crt_common.wgsl"

// Stage D - glow.
//
// Light scattering on its way out, at three scales, because one Gaussian cannot
// do this job and stacking a single radius is why most CRT bloom looks like a
// Photoshop filter. Level 0 is scatter inside the phosphor layer, level 1 is
// halation - light reflecting off the back of the front glass and re-exciting
// phosphor nearby - and level 2 is veiling glare, the whole screen lifting near
// a bright area.
//
// The pyramid is built from the *emission*, before the mask. That is both more
// physically correct - light leaves the phosphor before it meets the aperture -
// and practically necessary: a glow carrying triad structure would alias badly.
// It also gives the beam limiter its screen average for free.

@group(0) @binding(2) var inputTexture : texture_2d<f32>;

@fragment
fn crtFragment(@location(0) uv : vec2<f32>) -> @location(0) vec4<f32> {
    let level = u32(crt.glowRadius.w + 0.5);
    var radius = crt.glowRadius.x;
    if (level == 1u) {
        radius = crt.glowRadius.y;
    } else if (level == 2u) {
        radius = crt.glowRadius.z;
    }

    // The radius is authored in output pixels, so the tap offset is simply that
    // as a fraction of the output extent - the level's own resolution already
    // supplies the wide support, since it is the product of repeated quarter
    // downsampling. Scaling the step by the level as well double-counts that and
    // pushes level 2's taps clean off the texture, which silently turns the
    // coarsest level into a corner average and leaves the beam limiter reading
    // roughly nothing.
    let output = max(crt.extents0.zw, vec2<f32>(1.0, 1.0));
    let step = vec2<f32>(max(radius, 1.0), max(radius, 1.0)) / output;

    var accumulated = vec3<f32>(0.0, 0.0, 0.0);
    var weightSum = 0.0;
    // Unrolled: naga rejects dynamic vec4 indexing, and a loop that only ever
    // returns reports "Function is invalid" with no line number.
    let offsets = array<vec2<f32>, 9>(
        vec2<f32>(-1.0, -1.0), vec2<f32>(0.0, -1.0), vec2<f32>(1.0, -1.0),
        vec2<f32>(-1.0,  0.0), vec2<f32>(0.0,  0.0), vec2<f32>(1.0,  0.0),
        vec2<f32>(-1.0,  1.0), vec2<f32>(0.0,  1.0), vec2<f32>(1.0,  1.0));
    let weights = array<f32, 9>(0.0625, 0.125, 0.0625, 0.125, 0.25, 0.125, 0.0625, 0.125, 0.0625);
    for (var index : i32 = 0; index < 9; index = index + 1) {
        let tap = clamp(uv + offsets[index] * step, vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 1.0));
        accumulated = accumulated + textureSample(inputTexture, crtSampler, tap).rgb * weights[index];
        weightSum = weightSum + weights[index];
    }
    return vec4<f32>(accumulated / max(weightSum, 1.0e-5), 1.0);
}
