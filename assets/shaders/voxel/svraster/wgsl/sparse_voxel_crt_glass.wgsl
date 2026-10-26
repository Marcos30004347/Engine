#include "sparse_voxel_crt_common.wgsl"

// Stage E - the glass.
//
// The front of the tube and the room in front of it. Everything optical that
// happens after the light leaves the phosphor, and the tonemap last.

@group(0) @binding(2) var tubeTexture : texture_2d<f32>;
@group(0) @binding(3) var glow0Texture : texture_2d<f32>;
@group(0) @binding(4) var glow1Texture : texture_2d<f32>;
@group(0) @binding(5) var glow2Texture : texture_2d<f32>;

fn glassIsNeutral() -> bool {
    return crt.glowWeight.x == 0.0 && crt.glowWeight.y == 0.0 && crt.glowWeight.z == 0.0 &&
           crt.glassA.x == 0.0 && crt.glassA.y == 0.0 && crt.glassA.z == 0.0 && crt.glassA.w == 0.0 &&
           crt.roll.x < 1.5 && crt.colorA.x == 1.0 && crt.colorA.y == 1.0 && crt.colorA.z == 1.0 &&
           crt.colorA.w == 1.0 && crt.colorB.x == 0.0;
}

@fragment
fn crtFragment(@location(0) uv : vec2<f32>) -> @location(0) vec4<f32> {
    if (glassIsNeutral()) {
        return textureSample(tubeTexture, crtSampler, uv);
    }
    let bezel = crt.bezel.rgb;

    // The tube face, antialiased against its own screen-space derivative. Left
    // hard, the rounded corners crawl badly under any camera motion.
    let distance = crtTubeDistance(uv);
    let edgeWidth = max(fwidth(distance), 1.0e-5);
    let inside = 1.0 - smoothstep(-edgeWidth, edgeWidth, distance);
    if (inside <= 0.0) {
        return vec4<f32>(bezel, 1.0);
    }

    var colour = textureSample(tubeTexture, crtSampler, uv).rgb;

    // Glow composite. Halation is tinted warm because it is light that has been
    // through the glass twice.
    colour = colour + textureSample(glow0Texture, crtSampler, uv).rgb * crt.glowWeight.x;
    colour = colour + textureSample(glow1Texture, crtSampler, uv).rgb * crt.glowWeight.y * crt.halationTint.rgb;
    colour = colour + textureSample(glow2Texture, crtSampler, uv).rgb * crt.glowWeight.z * vec3<f32>(0.88, 0.94, 1.0);

    // Glass tint, modelled as the asymmetry it actually is: emitted light passes
    // the tint once on its way out, but anything reflected passes it twice. That
    // is why tint improves contrast far more than it dims the picture, and why
    // this is not a brightness slider.
    let tint = clamp(crt.glassA.x, 0.0, 1.0);
    colour = colour * (1.0 - tint * 0.45);

    if (crt.glassA.y > 0.0) {
        // A soft room light off the upper left of the glass. Subtle by default;
        // a screen with no reflection at all reads as a flat texture rather than
        // as glass.
        let p = uv * 2.0 - vec2<f32>(1.0, 1.0);
        let offset = vec2<f32>(p.x + 0.45, p.y + 0.52);
        let falloff = offset.x * offset.x * 1.5 + offset.y * offset.y * 2.4;
        let strength = exp(-falloff * 2.1) * crt.glassA.y * (1.0 - tint) * (1.0 - tint);
        colour = colour + strength * vec3<f32>(0.34, 0.36, 0.40);
    }

    // Roll bar: the out-of-sync refresh artifact. Unlike the beam variant it
    // modulates the finished picture, so it lives here and never reaches the
    // persistence ring.
    if (crt.roll.x > 1.5) {
        let age = crtBeamAge(uv.y);
        let height = max(crt.roll.z, 0.01);
        var band = 0.0;
        if (age < height) {
            band = sin(age / height * 3.14159265);
        }
        colour = colour * (1.0 - crt.roll.w * 0.65 * band);
    }

    if (crt.glassA.w > 0.0) {
        let p = uv * 2.0 - vec2<f32>(1.0, 1.0);
        let vignette = 1.0 - crt.glassA.w * 0.85 * pow(clamp(dot(p, p) * 0.72, 0.0, 1.0), 1.6);
        colour = colour * vignette;
    }

    // Grade, then transfer. A CRT does not reach black, so the lift is
    // authentic rather than a concession.
    colour = colour * crt.colorA.y;
    colour = (colour - vec3<f32>(0.5, 0.5, 0.5)) * crt.colorA.z + vec3<f32>(0.5, 0.5, 0.5);
    let grey = crtLuminance(colour);
    colour = mix(vec3<f32>(grey, grey, grey), colour, crt.colorA.w);
    colour = max(colour, vec3<f32>(0.0, 0.0, 0.0)) + vec3<f32>(crt.colorB.x, crt.colorB.x, crt.colorB.x);
    if (crt.colorA.x != 1.0) {
        colour = pow(max(colour, vec3<f32>(0.0, 0.0, 0.0)), vec3<f32>(crt.colorA.x, crt.colorA.x, crt.colorA.x));
    }

    return vec4<f32>(mix(bezel, colour, inside), 1.0);
}
