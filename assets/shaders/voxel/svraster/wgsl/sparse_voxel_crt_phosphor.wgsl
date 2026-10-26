#include "sparse_voxel_crt_common.wgsl"

// Stage C - the phosphor.
//
// Where the beam meets the mask, and where subpixel blending happens. The naive
// mask multiplies each output pixel by (1,0,0), (0,1,0) or (0,0,1), which is
// only correct when exactly one stripe covers exactly one pixel. At any other
// magnification the spot straddles two or three stripes and the answer is an
// integral over the pixel footprint, not a lookup.
//
// Three things fall out of doing it properly: fractional pitches work, the mask
// fades out smoothly instead of aliasing into rainbow moire as the pitch
// approaches one pixel, and a spot wider than a triad blends across all three
// and reads white - which is the real relationship between focus and colour.

@group(0) @binding(2) var emissionTexture : texture_2d<f32>;
@group(0) @binding(3) var averageTexture : texture_2d<f32>;

// Integral of a periodic stripe indicator from 0 to x, where the stripe occupies
// [a, b) inside every period. Exact, which is what makes fractional pitch and
// the smooth fade-out work.
fn crtStripeIntegral(x : f32, period : f32, a : f32, b : f32) -> f32 {
    let whole = floor(x / period);
    let rest = x - whole * period;
    return whole * (b - a) + clamp(rest, a, b) - a;
}

// Coverage of one phosphor stripe over the output pixel footprint [x, x+1),
// normalised so a fully blurred mask returns exactly 1.0.
fn crtStripeCoverage(x : f32, period : f32, stripeStart : f32, lit : f32) -> f32 {
    let a = stripeStart;
    let b = stripeStart + lit;
    let covered = crtStripeIntegral(x + 1.0, period, a, b) - crtStripeIntegral(x, period, a, b);
    let mean = lit / period;
    return covered / max(mean, 1.0e-5);
}

fn phosphorIsNeutral() -> bool {
    let maskOff = crt.maskA.x < 0.5 || crt.maskA.z == 0.0;
    return maskOff && crt.maskB.x == 0.0 && crt.maskB.y == 0.0 && crt.maskB.z == 0.0 && crt.limiter.x == 0.0;
}

@fragment
fn crtFragment(@location(0) uv : vec2<f32>) -> @location(0) vec4<f32> {
    if (phosphorIsNeutral()) {
        return textureSample(emissionTexture, crtSampler, uv);
    }
    let output = max(crt.extents0.zw, vec2<f32>(1.0, 1.0));

    // Convergence: the three guns do not land in the same place, and the error
    // grows toward the edges. Cheap, and instantly recognisable.
    let offset = crt.maskB.x * (uv.x * 2.0 - 1.0) / output.x;
    var colour = vec3<f32>(0.0, 0.0, 0.0);
    if (crt.maskB.x == 0.0) {
        colour = textureSample(emissionTexture, crtSampler, uv).rgb;
    } else {
        colour = vec3<f32>(
            textureSample(emissionTexture, crtSampler, vec2<f32>(uv.x - offset, uv.y)).r,
            textureSample(emissionTexture, crtSampler, uv).g,
            textureSample(emissionTexture, crtSampler, vec2<f32>(uv.x + offset, uv.y)).b);
    }

    // Automatic beam limiter. A real set cannot drive the whole screen at full
    // brightness - the supply sags and the entire picture dims. This is why a
    // small highlight on a dark field is far brighter than a full white field,
    // which no static tonemap reproduces. The mean comes from the coarsest glow
    // level, which is already a screen-wide average of the emitted light.
    if (crt.limiter.x > 0.0) {
        // A four-tap probe is not a mean - it is four points, and on a mostly
        // dark scene all four land in shadow and the limiter reads zero. A 4x4
        // grid over an already heavily downsampled level is close enough to the
        // screen average that the knee means what it says.
        var total = vec3<f32>(0.0, 0.0, 0.0);
        for (var y : i32 = 0; y < 4; y = y + 1) {
            for (var x : i32 = 0; x < 4; x = x + 1) {
                let probe = vec2<f32>((f32(x) + 0.5) * 0.25, (f32(y) + 0.5) * 0.25);
                total = total + textureSample(averageTexture, crtSampler, probe).rgb;
            }
        }
        let mean = crtLuminance(total / 16.0);
        colour = colour / (1.0 + crt.limiter.x * 2.2 * max(0.0, mean - crt.limiter.y));
    }

    // The mask itself.
    let maskType = u32(crt.maskA.x + 0.5);
    if (maskType > 0u && crt.maskA.z > 0.0) {
        let pitch = max(crt.maskA.y, 0.25);
        let stripe = pitch / 3.0;
        // The black matrix between stripes emits nothing. This is where a real
        // tube's contrast comes from, and it is a separate term from the stripe
        // width rather than a darkening applied on top.
        let lit = stripe * (1.0 - clamp(crt.maskA.w, 0.0, 0.9));

        // The footprint is [column, column + 1), so it has to be anchored at the
        // pixel's left edge. `uv.x * output.x` is its *centre*, which offsets
        // every footprint by half a pixel and makes every pixel straddle two
        // stripes even at a pitch that divides the grid exactly - the mask then
        // never reaches full separation and its contrast wobbles with pitch
        // instead of holding a plateau.
        var column = uv.x * output.x - 0.5;
        let row = uv.y * output.y - 0.5;
        if (maskType == 2u) {
            // Shadow mask: alternate row bands offset by half a triad, which is
            // what turns stripes into staggered dots.
            let band = floor(row / (pitch * 1.5));
            column = column + fract(band * 0.5) * 2.0 * stripe * 1.5;
        }

        let coverR = crtStripeCoverage(column, pitch, 0.0, lit);
        let coverG = crtStripeCoverage(column, pitch, stripe, lit);
        let coverB = crtStripeCoverage(column, pitch, stripe * 2.0, lit);
        var mask = vec3<f32>(
            1.0 + crt.maskA.z * (coverR - 1.0),
            1.0 + crt.maskA.z * (coverG - 1.0),
            1.0 + crt.maskA.z * (coverB - 1.0));

        if (maskType == 3u) {
            // Slot mask: the grille broken by a horizontal bridge every other
            // pitch, so the stripes become slots.
            let slotPhase = fract(row / (pitch * 2.0));
            if (slotPhase < 0.09) {
                mask = mask * (1.0 - crt.maskA.z * 0.8);
            }
        }

        // Put back what the black matrix removed, so raising it trades highlight
        // headroom for contrast instead of simply dimming the picture.
        let compensation = 1.0 / max(1.0 - crt.maskA.z * clamp(crt.maskA.w, 0.0, 0.9) * 0.85, 0.1);
        colour = colour * mask * compensation;
    }

    colour = crtPhosphorGamut(colour, crt.maskB.y);

    // Phosphor saturation: emission is sub-linear at high excitation, so
    // highlights compress toward the phosphor's own colour rather than clipping
    // to white. Rescaled so the low end is unchanged.
    let saturation = clamp(crt.maskB.z, 0.0, 1.0);
    if (saturation > 0.0) {
        let knee = 0.28 * saturation;
        colour = mix(colour, colour / (vec3<f32>(1.0, 1.0, 1.0) + knee * colour) * (1.0 + knee), 1.0);
    }

    return vec4<f32>(colour, 1.0);
}
