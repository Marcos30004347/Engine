#include "sparse_voxel_crt_common.wgsl"

// Stage A - the signal.
//
// The video path between the console and the tube. RGB is a clean pass-through
// and the shipping default; S-video and composite are real encode/decode chains
// through Y'IQ. Dot crawl and rainbow banding are not authored - they fall out
// of decoding a composite signal whose chroma has leaked into luma, which is
// what the crosstalk term below reproduces.

@group(0) @binding(2) var inputTexture : texture_2d<f32>;

fn tap(uv : vec2<f32>, dx : f32, internal : vec2<f32>) -> vec3<f32> {
    let offset = vec2<f32>(dx / internal.x, 0.0);
    return textureSample(inputTexture, crtSampler, clamp(uv + offset, vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 1.0))).rgb;
}

// An RGB feed carries no chroma subcarrier, so the chroma bandwidth dial has
// nothing to act on there - only luma bandwidth, ringing and noise, which are
// properties of the cable and the amplifier rather than of the encoding. That is
// what separates the three paths: RGB has no chroma band-limiting, S-video has
// it, and composite adds the luma/chroma crosstalk on top.
fn signalIsNeutral() -> bool {
    let path = u32(crt.signalA.x + 0.5);
    let chromaNeutral = path == 0u || crt.signalA.z >= 1.0;
    return crt.signalA.y >= 1.0 && chromaNeutral && crt.signalA.w == 0.0 && crt.signalB.y == 0.0;
}

@fragment
fn crtFragment(@location(0) uv : vec2<f32>) -> @location(0) vec4<f32> {
    // The RGB path with full bandwidth is exactly the input. Bit-exact, which
    // is what "the signal stage is a no-op" rests on.
    if (signalIsNeutral()) {
        return textureSample(inputTexture, crtSampler, uv);
    }

    let internal = max(crt.extents0.xy, vec2<f32>(1.0, 1.0));
    let path = u32(crt.signalA.x + 0.5);

    // Luma and chroma are band-limited independently: chroma bandwidth is what
    // makes red bleed sideways, luma bandwidth is what softens detail. On a
    // real set they are wildly different numbers, which is why one dial cannot
    // stand in for both.
    var chromaBandwidth = clamp(crt.signalA.z, 0.0, 1.0);
    if (path == 0u) {
        chromaBandwidth = 1.0;
    }
    let chromaTaps = 1.0 + (1.0 - chromaBandwidth) * 9.0;
    var chroma = vec2<f32>(0.0, 0.0);
    var chromaWeight = 0.0;
    for (var index : i32 = -5; index <= 5; index = index + 1) {
        let distance = abs(f32(index));
        if (distance > chromaTaps) {
            continue;
        }
        let weight = 1.0 - distance / (chromaTaps + 1.0);
        let yiq = crtRgbToYiq(tap(uv, f32(index), internal));
        chroma = chroma + yiq.yz * weight;
        chromaWeight = chromaWeight + weight;
    }
    chroma = chroma / max(chromaWeight, 1.0e-5);

    let lumaTaps = 1.0 + (1.0 - clamp(crt.signalA.y, 0.0, 1.0)) * 3.0;
    var luma = 0.0;
    var lumaWeight = 0.0;
    for (var index : i32 = -3; index <= 3; index = index + 1) {
        let distance = abs(f32(index));
        if (distance > lumaTaps) {
            continue;
        }
        let weight = 1.0 - distance / (lumaTaps + 1.0);
        luma = luma + crtRgbToYiq(tap(uv, f32(index), internal)).x * weight;
        lumaWeight = lumaWeight + weight;
    }
    luma = luma / max(lumaWeight, 1.0e-5);

    // Amplifier overshoot. A bright edge gets a dark pre-echo and a bright
    // post-echo; one of the most recognisable CRT cues, and it costs a kernel
    // with a negative lobe rather than anything clever.
    if (crt.signalA.w > 0.0) {
        let before = crtRgbToYiq(tap(uv, -2.0, internal)).x;
        let after = crtRgbToYiq(tap(uv, 2.0, internal)).x;
        luma = luma + crt.signalA.w * (0.42 * (luma - before) - 0.22 * (after - luma));
    }

    // Composite carries chroma on a subcarrier that the decoder cannot fully
    // separate from luma. The residue alternates phase per line and per pixel,
    // which is exactly what dot crawl looks like on a saturated vertical edge.
    // S-video keeps the two on separate wires and shows none of it.
    if (path > 1u) {
        let row = floor(uv.y * internal.y);
        let column = floor(uv.x * internal.x);
        let phase = 1.0 - 2.0 * fract((row + column) * 0.5) * 2.0;
        luma = luma + crt.signalB.x * 0.16 * phase * length(chroma);
    }

    // Analog noise lives in the shadows, not the highlights, so it is weighted
    // by inverse luma rather than added flat.
    if (crt.signalB.y > 0.0) {
        let seed = uv * internal + vec2<f32>(crt.frame.y * 17.0, crt.frame.y * 31.0);
        let hashed = fract(sin(dot(seed, vec2<f32>(12.9898, 78.233))) * 43758.5453);
        luma = luma + (hashed - 0.5) * crt.signalB.y * 0.12 * (1.0 - clamp(luma, 0.0, 1.0));
    }

    let decoded = crtYiqToRgb(vec3<f32>(luma, chroma.x, chroma.y));
    return vec4<f32>(max(decoded, vec3<f32>(0.0, 0.0, 0.0)), 1.0);
}
