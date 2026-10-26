// Phase 1 of the FSR 3.1 upscaling plan: turn "less aliasing" into a number.
//
// Two quantities, reduced over the final upscaled image into a handful of
// atomics. They are deliberately cheap and deliberately *not* a quality score -
// neither is meaningful on its own, and they are only comparable between runs
// of the same scene and camera path. What they are good for is the one thing
// that matters here: telling whether a change to the accumulator moved the
// image in the intended direction, rather than judging it by eye.
//
//   - temporal instability: mean absolute difference against the previous
//     frame's accumulated colour. On a static camera a converged upscaler
//     drives this toward zero; what is left is shimmer.
//   - edge energy: mean Laplacian magnitude. Aliasing is high-frequency energy,
//     so a well anti-aliased image has less of it than an aliased one of the
//     same scene. It also rises with genuine sharpness, which is why it is
//     always read alongside the instability figure - sharpening that trades
//     shimmer for detail moves both, while aliasing moves only this one.
//
// Both are accumulated as fixed point because an atomic float add is not
// available; the scale is chosen so a full-screen sum stays inside 32 bits.
//
// Every figure is *also* reduced per screen region on a 4x4 grid, and that is
// not a nicety. A screen-wide mean is blind to a localised defect by
// construction: a band covering a fifth of the screen moves the mean by a fifth
// of its severity, which lands inside the run-to-run noise of every number in
// this file. The renderer has a standing bug that shows as wrong lighting over
// the right ~20% of the screen and it does not register on any whole-frame mean
// here. One region row makes it obvious which fifth is broken.

struct TemporalMetricsSettings {
    // xy display dimensions, z accumulator-term reduction enabled, w unused.
    dimensions : vec4<u32>,
}

@group(0) @binding(0) var<uniform> settings : TemporalMetricsSettings;
@group(0) @binding(1) var currentTexture : texture_2d<f32>;
@group(0) @binding(2) var previousTexture : texture_2d<f32>;
@group(0) @binding(3) var<storage, read_write> metrics : array<atomic<u32>>;
// The accumulator's history-weight term chain, one factor per channel. See
// `fsr2-accumulate-cs.wgsl`. Placeholders when the accumulator is not writing
// them, in which case the reduction below is skipped.
@group(0) @binding(4) var termsATexture : texture_2d<f32>;
@group(0) @binding(5) var termsBTexture : texture_2d<f32>;

const TEMPORAL_METRICS_SCALE : f32 = 4096.0;
const TEMPORAL_METRICS_INSTABILITY : u32 = 0u;
const TEMPORAL_METRICS_EDGE : u32 = 1u;
const TEMPORAL_METRICS_PIXELS : u32 = 2u;
const TEMPORAL_METRICS_EDGE_PIXELS : u32 = 3u;
const TEMPORAL_METRICS_TERMS : u32 = 4u;

// Per-region reduction, over a 4x4 grid of the screen starting at word 12.
// Six sums per region: instability, edge energy, pixels, edge pixels, then
// luminance and the count of pixels carrying any.
//
// The last two are what see a *stable* defect. Instability and edge energy are
// both differences - a region that is uniformly, persistently wrong reads zero
// on each of them, which is exactly what a black band stable in screen space
// looks like to every metric in this file. Mean luminance is the one figure
// that says "this fifth of the screen is dark and the rest is not".
const TEMPORAL_METRICS_REGION_AXIS : u32 = 4u;
const TEMPORAL_METRICS_REGION_COUNT : u32 = 16u;
const TEMPORAL_METRICS_REGION_WORDS : u32 = 6u;
const TEMPORAL_METRICS_REGIONS : u32 = 12u;
const TEMPORAL_METRICS_WORDS : u32 = 108u;
// Luminance is compressed to [0,1) before scaling, so a full region's sum stays
// far inside 32 bits at this scale.
const TEMPORAL_METRICS_LUMINANCE_SCALE : f32 = 1024.0;
// Below this compressed luminance a pixel counts as carrying no light. Chosen
// well under anything the renderer produces on a lit surface, so the figure
// separates "dark" from "lit" rather than "dim" from "bright".
const TEMPORAL_METRICS_LIT_THRESHOLD : f32 = 0.002;

fn metrics_region(pixel : vec2<u32>, dimensions : vec2<u32>) -> u32 {
    let cell = min(
        pixel * TEMPORAL_METRICS_REGION_AXIS / max(dimensions, vec2<u32>(1u)),
        vec2<u32>(TEMPORAL_METRICS_REGION_AXIS - 1u));
    return (cell.y * TEMPORAL_METRICS_REGION_AXIS + cell.x) *
        TEMPORAL_METRICS_REGION_WORDS + TEMPORAL_METRICS_REGIONS;
}

// The term factors sit in [0,1] and are summed over every pixel, so the scale
// has to keep a full-screen sum inside 32 bits: at 4K a factor of 1.0 everywhere
// is 8.3M * scale, which caps the scale near 512. 256 leaves headroom and still
// resolves each factor to 0.4%, far finer than the differences being looked for.
const TEMPORAL_TERM_SCALE : f32 = 256.0;

// Laplacian magnitude above which a pixel counts as an edge.
//
// Averaging the Laplacian over every pixel does not work, and the first version
// of this metric proved it: the figure did not move at all between a jittered
// and an unjittered render. Sponza is overwhelmingly smooth surface, so the
// aliased minority is diluted into nothing by the flat majority. Restricting
// the average to pixels that are actually edges is what makes the number about
// edges.
const TEMPORAL_METRICS_EDGE_THRESHOLD : f32 = 0.02;

fn metrics_luminance(value : vec3<f32>) -> f32 {
    return dot(value, vec3<f32>(0.2126, 0.7152, 0.0722));
}

// Tone mapped before differencing. The image is HDR, so a raw difference is
// dominated by whichever few pixels are brightest and says nothing about the
// edges the eye actually reads as aliased.
fn metrics_compress(value : f32) -> f32 {
    return value / (1.0 + max(value, 0.0));
}

@compute @workgroup_size(8, 8, 1)
fn computeTemporalMetrics(@builtin(global_invocation_id) gid : vec3<u32>) {
    let dimensions = vec2<i32>(max(settings.dimensions.xy, vec2<u32>(1u)));
    if (any(vec2<i32>(gid.xy) >= dimensions)) { return; }
    let pixel = vec2<i32>(gid.xy);

    let region = metrics_region(gid.xy, settings.dimensions.xy);

    let current = metrics_compress(
        metrics_luminance(textureLoad(currentTexture, pixel, 0).xyz));
    let previous = metrics_compress(
        metrics_luminance(textureLoad(previousTexture, pixel, 0).xyz));
    let instability = u32(clamp(
        abs(current - previous) * TEMPORAL_METRICS_SCALE + 0.5, 0.0, 2.0e9));
    atomicAdd(&metrics[TEMPORAL_METRICS_INSTABILITY], instability);
    atomicAdd(&metrics[region + 0u], instability);
    // Every pixel, not just the Laplacian's interior ones: this is the
    // denominator for the region's instability, and the two edge sums below
    // normalise against each other.
    atomicAdd(&metrics[region + 2u], 1u);
    atomicAdd(
        &metrics[region + 4u],
        u32(clamp(current * TEMPORAL_METRICS_LUMINANCE_SCALE + 0.5,
                  0.0, 2.0e9)));
    if (current > TEMPORAL_METRICS_LIT_THRESHOLD) {
        atomicAdd(&metrics[region + 5u], 1u);
    }

    if (settings.dimensions.z != 0u) {
        let termsA = textureLoad(termsATexture, pixel, 0);
        let termsB = textureLoad(termsBTexture, pixel, 0);
        for (var channel = 0u; channel < 4u; channel++) {
            atomicAdd(
                &metrics[TEMPORAL_METRICS_TERMS + channel],
                u32(clamp(termsA[channel] * TEMPORAL_TERM_SCALE + 0.5,
                          0.0, 2.0e9)));
            atomicAdd(
                &metrics[TEMPORAL_METRICS_TERMS + 4u + channel],
                u32(clamp(termsB[channel] * TEMPORAL_TERM_SCALE + 0.5,
                          0.0, 2.0e9)));
        }
    }

    // Four-neighbour Laplacian. Border pixels are skipped rather than clamped:
    // a clamped edge reads as a step and would add a constant frame of energy
    // that has nothing to do with the image.
    if (pixel.x > 0 && pixel.y > 0 &&
        pixel.x < dimensions.x - 1 && pixel.y < dimensions.y - 1) {
        var laplacian = 4.0 * current;
        laplacian -= metrics_compress(metrics_luminance(
            textureLoad(currentTexture, pixel + vec2<i32>(1, 0), 0).xyz));
        laplacian -= metrics_compress(metrics_luminance(
            textureLoad(currentTexture, pixel + vec2<i32>(-1, 0), 0).xyz));
        laplacian -= metrics_compress(metrics_luminance(
            textureLoad(currentTexture, pixel + vec2<i32>(0, 1), 0).xyz));
        laplacian -= metrics_compress(metrics_luminance(
            textureLoad(currentTexture, pixel + vec2<i32>(0, -1), 0).xyz));
        let magnitude = abs(laplacian);
        atomicAdd(&metrics[TEMPORAL_METRICS_PIXELS], 1u);
        if (magnitude > TEMPORAL_METRICS_EDGE_THRESHOLD) {
            let energy = u32(clamp(
                magnitude * TEMPORAL_METRICS_SCALE + 0.5, 0.0, 2.0e9));
            atomicAdd(&metrics[TEMPORAL_METRICS_EDGE], energy);
            atomicAdd(&metrics[TEMPORAL_METRICS_EDGE_PIXELS], 1u);
            atomicAdd(&metrics[region + 1u], energy);
            atomicAdd(&metrics[region + 3u], 1u);
        }
    }
}

@compute @workgroup_size(64, 1, 1)
fn resetTemporalMetrics(@builtin(global_invocation_id) gid : vec3<u32>) {
    if (gid.x >= TEMPORAL_METRICS_WORDS) { return; }
    atomicStore(&metrics[gid.x], 0u);
}
