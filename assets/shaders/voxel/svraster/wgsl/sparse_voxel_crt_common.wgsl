// Shared CRT simulation uniforms and helpers.
//
// See docs/crt-post-process-plan.md. The chain is five stages in physical
// order - signal, beam, phosphor, glow, glass - and this file carries the one
// parameter block all of them read, the colour matrices, the deflection warp,
// and the fullscreen vertex stage.
//
// Two rules for anything added here:
//   - vec4 blocks only. A vec3<bool> member becomes short3 in MSL and fails
//     pipeline creation rather than compilation, with no useful message.
//   - the colour matrices live here and nowhere else. Two stages disagreeing
//     about a matrix is a defect that reads as a tint bug.

struct CRTUniforms {
    // internal width, internal height, output width, output height
    extents0 : vec4<f32>,
    // window width, window height, stage index, debug stage
    extents1 : vec4<f32>,
    // time seconds, frame index, pixel aspect, integer scale
    frame : vec4<f32>,
    // signal path (0 rgb, 1 s-video, 2 composite), luma bw, chroma bw, ringing
    signalA : vec4<f32>,
    // dot crawl, noise, unused, unused
    signalB : vec4<f32>,
    // curvature x, curvature y, pincushion, overscan
    geometryA : vec4<f32>,
    // linearity x, linearity y, rotation radians, unused
    geometryB : vec4<f32>,
    // spot width min, spot width max, spot height min, spot height max
    spotA : vec4<f32>,
    // spot skew, interlace mode, persistence, unused
    spotB : vec4<f32>,
    // per-channel persistence, unused
    persistence : vec4<f32>,
    // mask type (0 none, 1 grille, 2 shadow, 3 slot), pitch, strength, black matrix
    maskA : vec4<f32>,
    // convergence, phosphor gamut blend, phosphor saturation, unused
    maskB : vec4<f32>,
    // beam limiter, beam limiter knee, unused, unused
    limiter : vec4<f32>,
    // glow weights level 0/1/2, unused
    glowWeight : vec4<f32>,
    // glow radii level 0/1/2, and w carries this dispatch's pyramid level
    glowRadius : vec4<f32>,
    // halation tint rgb, unused
    halationTint : vec4<f32>,
    // glass tint, reflection, corner radius, vignette
    glassA : vec4<f32>,
    // bezel colour rgb, unused
    bezel : vec4<f32>,
    // gamma, brightness, contrast, saturation
    colorA : vec4<f32>,
    // black level, unused, unused, unused
    colorB : vec4<f32>,
    // roll mode (0 off, 1 beam, 2 rollbar), speed, height, strength
    roll : vec4<f32>,
}

@group(0) @binding(0) var<uniform> crt : CRTUniforms;
@group(0) @binding(1) var crtSampler : sampler;

struct CRTVertexOutput {
    @builtin(position) position : vec4<f32>,
    @location(0) uv : vec2<f32>,
};

@vertex
fn crtVertex(@builtin(vertex_index) vertexIndex : u32) -> CRTVertexOutput {
    let cornerIndices = array<u32, 6>(0u, 2u, 1u, 1u, 2u, 3u);
    let positions = array<vec2<f32>, 4>(
        vec2<f32>(-1.0, -1.0),
        vec2<f32>( 1.0, -1.0),
        vec2<f32>(-1.0,  1.0),
        vec2<f32>( 1.0,  1.0));
    let uvs = array<vec2<f32>, 4>(
        vec2<f32>(0.0, 0.0),
        vec2<f32>(1.0, 0.0),
        vec2<f32>(0.0, 1.0),
        vec2<f32>(1.0, 1.0));
    let corner = cornerIndices[vertexIndex];
    var output : CRTVertexOutput;
    output.position = vec4<f32>(positions[corner], 0.0, 1.0);
    output.uv = uvs[corner];
    return output;
}

fn crtLuminance(colour : vec3<f32>) -> f32 {
    return dot(colour, vec3<f32>(0.299, 0.587, 0.114));
}

fn crtRgbToYiq(colour : vec3<f32>) -> vec3<f32> {
    return vec3<f32>(
        dot(colour, vec3<f32>(0.299, 0.587, 0.114)),
        dot(colour, vec3<f32>(0.5959, -0.2746, -0.3213)),
        dot(colour, vec3<f32>(0.2115, -0.5227, 0.3112)));
}

fn crtYiqToRgb(yiq : vec3<f32>) -> vec3<f32> {
    return vec3<f32>(
        yiq.x + 0.956 * yiq.y + 0.619 * yiq.z,
        yiq.x - 0.272 * yiq.y - 0.647 * yiq.z,
        yiq.x - 1.106 * yiq.y + 1.703 * yiq.z);
}

// sRGB primaries towards P22 phosphor primaries. P22 red is deeper and less
// orange than sRGB red, P22 green is markedly less pure, and P22 blue sits
// closer to violet. This is why CRT colour reads differently rather than merely
// "more saturated"; blending toward it is what `phosphorGamut` does.
fn crtPhosphorGamut(colour : vec3<f32>, amount : f32) -> vec3<f32> {
    let converted = vec3<f32>(
        dot(colour, vec3<f32>(1.0655, -0.0554, -0.0101)),
        dot(colour, vec3<f32>(-0.0196, 1.0364, -0.0168)),
        dot(colour, vec3<f32>(0.0026, -0.0429, 1.0403)));
    return mix(colour, converted, clamp(amount, 0.0, 1.0));
}

// Deflection. Barrel plus pincushion, linearity, rotation and overscan, mapping
// a point on the glass back to a point in the signal. Exactly the identity when
// every dial is neutral - the early-out is not an optimisation, it is what
// makes "curvature = 0 is a no-op" hold bit-exactly rather than approximately.
fn crtDeflectionIsNeutral() -> bool {
    return crt.geometryA.x == 0.0 && crt.geometryA.y == 0.0 && crt.geometryA.z == 0.0 &&
           crt.geometryA.w == 1.0 && crt.geometryB.x == 0.0 && crt.geometryB.y == 0.0 &&
           crt.geometryB.z == 0.0;
}

fn crtWarp(uv : vec2<f32>) -> vec2<f32> {
    if (crtDeflectionIsNeutral()) {
        return uv;
    }
    var p = uv * 2.0 - vec2<f32>(1.0, 1.0);

    let rotation = crt.geometryB.z;
    if (rotation != 0.0) {
        let s = sin(rotation);
        let c = cos(rotation);
        p = vec2<f32>(p.x * c - p.y * s, p.x * s + p.y * c);
    }

    // Linearity error: real sets stretch near the edges because the deflection
    // is not perfectly linear in the yoke current. Cubic, so the centre is
    // untouched and the sign selects stretch or compression.
    p = vec2<f32>(
        p.x + crt.geometryB.x * p.x * (1.0 - p.x * p.x),
        p.y + crt.geometryB.y * p.y * (1.0 - p.y * p.y));

    // Barrel is per-axis; pincushion is the radial term that opposes it.
    let px = p.x;
    let py = p.y;
    let r2 = px * px + py * py;
    p = vec2<f32>(
        px * (1.0 + crt.geometryA.x * py * py + crt.geometryA.z * r2),
        py * (1.0 + crt.geometryA.y * px * px + crt.geometryA.z * r2));

    // The deflection always drove past the visible glass, so the outer few
    // percent of the signal was never seen.
    p = p / max(crt.geometryA.w, 0.01);
    return p * 0.5 + vec2<f32>(0.5, 0.5);
}

// Distance outside the tube face, in normalised units, with rounded corners.
// Negative inside. Antialiased by the caller against its own derivative, or the
// corners crawl badly under any camera motion.
fn crtTubeDistance(uv : vec2<f32>) -> f32 {
    let radius = clamp(crt.glassA.z, 0.0, 0.45);
    let p = abs(uv * 2.0 - vec2<f32>(1.0, 1.0));
    if (radius <= 0.0) {
        return max(p.x, p.y) - 1.0;
    }
    let inner = vec2<f32>(1.0 - radius, 1.0 - radius);
    let d = p - inner;
    return length(max(d, vec2<f32>(0.0, 0.0))) + min(max(d.x, d.y), 0.0) - radius;
}

// The beam's vertical position this instant, in [0, 1), driven by wall clock so
// it does not change speed with frame rate. At 60 Hz one frame shows one beam
// position, so this is a stylised sweep rather than a physical one.
fn crtBeamPosition() -> f32 {
    return fract(crt.frame.x * crt.roll.y);
}

// How far below the beam a point on the glass sits, wrapped into [0, 1).
fn crtBeamAge(v : f32) -> f32 {
    return fract(v - crtBeamPosition());
}
