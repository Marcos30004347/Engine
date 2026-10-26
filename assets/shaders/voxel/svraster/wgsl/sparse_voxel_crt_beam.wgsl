#include "sparse_voxel_crt_common.wgsl"

// Stage B - the beam.
//
// Deflection and excitation, writing the light the phosphor layer is emitting
// before any mask geometry. Scanlines are not a dial here: they are what happens
// when the vertical spot is narrower than the line pitch, so raising the spot
// height closes the gaps on its own, exactly as turning up the brightness does
// on a real set.

@group(0) @binding(2) var signalTexture : texture_2d<f32>;
@group(0) @binding(3) var previousEmission : texture_2d<f32>;

fn signalTap(coordinate : vec2<f32>, internal : vec2<f32>) -> vec3<f32> {
    let uv = (coordinate + vec2<f32>(0.5, 0.5)) / internal;
    return textureSample(signalTexture, crtSampler, clamp(uv, vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 1.0))).rgb;
}

// Every dial this stage owns at its identity value. A zero spot is a point
// beam, which reproduces the signal exactly - so this is the physical reading of
// "no beam simulation", not a special case bolted on beside it.
fn beamIsNeutral() -> bool {
    return crtDeflectionIsNeutral() && crt.spotA.x == 0.0 && crt.spotA.y == 0.0 &&
           crt.spotA.z == 0.0 && crt.spotA.w == 0.0 && crt.spotB.y == 0.0 &&
           crt.spotB.z == 0.0 && crt.roll.x < 0.5;
}

@fragment
fn crtFragment(@location(0) uv : vec2<f32>) -> @location(0) vec4<f32> {
    if (beamIsNeutral()) {
        return textureSample(signalTexture, crtSampler, uv);
    }
    let internal = max(crt.extents0.xy, vec2<f32>(1.0, 1.0));
    let warped = crtWarp(uv);

    // Off the glass entirely. Emission is zero there; the bezel colour is the
    // glass stage's business, not the beam's.
    if (warped.x < 0.0 || warped.x > 1.0 || warped.y < 0.0 || warped.y > 1.0) {
        return vec4<f32>(0.0, 0.0, 0.0, 1.0);
    }

    let centre = warped * internal - vec2<f32>(0.5, 0.5);
    let base = floor(centre);

    // Local luminance drives defocus, so the centre tap comes first: spot size
    // grows with beam current, which is why bright areas are physically larger
    // and softer rather than merely brighter.
    let centreColour = signalTap(base, internal);
    let drive = clamp(crtLuminance(centreColour), 0.0, 1.5);
    let spotWidth = max(mix(crt.spotA.x, crt.spotA.y, drive), 0.05);
    let spotHeight = max(mix(crt.spotA.z, crt.spotA.w, drive), 0.05);
    let widthSigma2 = 2.0 * spotWidth * spotWidth;
    let heightSigma2 = 2.0 * spotHeight * spotHeight;

    // Interlacing: one field per frame. The other field is not black, it is
    // whatever persistence still holds from last frame, which is exactly how a
    // real interlaced display works.
    let interlacing = crt.spotB.y > 0.5;
    let field = f32(u32(crt.frame.y) & 1u);

    var accumulated = vec3<f32>(0.0, 0.0, 0.0);
    var weightSum = 0.0;
    for (var dy : i32 = -1; dy <= 2; dy = dy + 1) {
        let lineY = base.y + f32(dy);
        if (interlacing && abs(fract(lineY * 0.5) * 2.0 - field) > 0.5) {
            continue;
        }
        let deltaY = lineY - centre.y;
        let weightY = exp(-(deltaY * deltaY) / heightSigma2);
        if (weightY < 0.002) {
            continue;
        }
        for (var dx : i32 = -2; dx <= 2; dx = dx + 1) {
            let sampleX = base.x + f32(dx);
            let deltaX = sampleX - centre.x;
            // Bright content smears right: the video amplifier cannot slew back
            // down as fast as it slewed up.
            var skew = 1.0;
            if (deltaX > 0.0) {
                skew = 1.0 + crt.spotB.x * drive;
            }
            let weightX = exp(-(deltaX * deltaX) / (widthSigma2 * skew * skew));
            let weight = weightX * weightY;
            if (weight < 0.002) {
                continue;
            }
            accumulated = accumulated + signalTap(vec2<f32>(sampleX, lineY), internal) * weight;
            weightSum = weightSum + weight;
        }
    }

    // Deliberately NOT normalised by the weights actually gathered. The energy
    // missing between two scan lines *is* the scanline: normalising it away is
    // how a beam model degenerates into a plain resample. The reference is the
    // fully covered case instead, clamped so a very tight spot cannot divide the
    // picture into nothing.
    let reference = max(6.2831853 * spotWidth * spotHeight, 0.35);
    var emission = accumulated / reference;
    if (interlacing) {
        emission = emission * 2.0;
    }

    // Roll, beam variant: a bright leading edge sweeping top to bottom with the
    // tube decaying behind it. The roll bar variant lives in the glass stage,
    // because it modulates the finished picture rather than the beam.
    if (crt.roll.x > 0.5 && crt.roll.x < 1.5) {
        let age = crtBeamAge(warped.y);
        let height = max(crt.roll.z, 0.01);
        var lead = 0.0;
        if (age < height) {
            lead = 1.0 - age / height;
        }
        let decay = 1.0 - crt.roll.w * 0.55 * (1.0 - pow(1.0 - age, 3.0));
        emission = emission * decay * (1.0 + crt.roll.w * 0.9 * lead * lead);
    }

    // Phosphor persistence. Charge stored at the phosphor, so it belongs here -
    // before the mask, which is static geometry and must never enter this ring
    // or it compounds every frame. max() rather than a sum: a trailing image
    // fades, it does not accumulate into a brighter one.
    let decay = clamp(crt.spotB.z, 0.0, 0.95) * clamp(crt.persistence.rgb, vec3<f32>(0.0), vec3<f32>(1.0));
    if (crt.spotB.z > 0.0) {
        let previous = textureSample(previousEmission, crtSampler, uv).rgb;
        emission = max(emission, previous * decay);
    }

    return vec4<f32>(emission, 1.0);
}
