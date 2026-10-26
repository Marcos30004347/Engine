struct SvgfAtrousUniforms {
    viewport: vec4<u32>,
    params0: vec4<f32>,
    params1: vec4<f32>,
}

@group(0) @binding(0) var<uniform> uniforms: SvgfAtrousUniforms;
@group(0) @binding(1) var sourceTexture: texture_2d<f32>;
@group(0) @binding(2) var depthTexture: texture_depth_2d;
@group(0) @binding(3) var normalTexture: texture_2d<f32>;
@group(0) @binding(4) var outputTexture: texture_storage_2d<rgba16float, write>;

fn enabled() -> bool {
    return uniforms.viewport.z != 0u;
}

fn step_width() -> i32 {
    return max(i32(uniforms.viewport.w), 1);
}

fn depth_sigma() -> f32 {
    return max(uniforms.params0.x, 1.0e-6);
}

fn normal_sigma() -> f32 {
    return max(uniforms.params0.y, 1.0e-4);
}

fn luminance_sigma() -> f32 {
    return max(uniforms.params0.z, 1.0e-4);
}

fn firefly_clamp() -> f32 {
    return max(uniforms.params0.w, 1.0e-4);
}

fn spatial_strength() -> f32 {
    return clamp(uniforms.params1.x, 0.0, 1.0);
}

fn variance_sigma() -> f32 {
    return max(uniforms.params1.y, 1.0e-4);
}

fn max_history_frames() -> f32 {
    return max(uniforms.params1.z, 1.0);
}

fn output_hit_mask() -> bool {
    return uniforms.params1.w != 0.0;
}

fn spatial_sample_valid(alpha: f32) -> bool {
    return alpha > 1.0e-5 || (alpha < -0.5 && alpha > -1.5);
}

fn normalize_normal(n: vec3<f32>) -> vec3<f32> {
    let len_sq = dot(n, n);
    return select(
        vec3<f32>(0.0, 1.0, 0.0), n * inverseSqrt(len_sq),
        len_sq > 1.0e-8);
}

fn load_normal(pixel: vec2<i32>) -> vec3<f32> {
    let n = textureLoad(normalTexture, pixel, 0).xyz;
    return normalize_normal(n);
}

fn luminance(value: vec3<f32>) -> f32 {
    return max(dot(value, vec3<f32>(0.2126, 0.7152, 0.0722)), 0.0);
}

fn clamp_luminance(value: vec3<f32>) -> vec3<f32> {
    let luma = luminance(value);
    return value * min(1.0, firefly_clamp() / max(luma, 1.0e-4));
}

// Remove the first-order depth slope before applying the bilateral depth
// weight. Raw device depth varies rapidly across oblique planar faces, so the
// old absolute comparison rejected every side-face tap even when normals
// matched. This retains edge rejection for deviations from the local
// plane while allowing the a-trous footprint to follow a sloped surface.
// left, right, up and down first-order slopes, computed once per output pixel.
fn robust_axis_depth_slope(
    negative_delta: f32, positive_delta: f32) -> f32 {
    let negative_magnitude = abs(negative_delta);
    let positive_magnitude = abs(positive_delta);
    let minimum_magnitude = min(negative_magnitude, positive_magnitude);
    let maximum_magnitude = max(negative_magnitude, positive_magnitude);
    // A plane crosses the center depth: its two signed differences point in
    // opposite directions and have comparable magnitudes. At a silhouette,
    // terrace edge, or thin voxel both neighbours instead lie on one side or
    // one delta is much larger. Returning zero in those cases prevents the
    // discontinuity from being extrapolated as a filterable plane slope.
    let planar = negative_delta * positive_delta < 0.0 &&
        maximum_magnitude <= minimum_magnitude * 4.0 + 1.0e-6;
    return select(
        0.0, 0.5 * (negative_magnitude + positive_magnitude), planar);
}

fn planar_depth_slopes(
    center_pixel: vec2<i32>, center_depth: f32) -> vec2<f32> {
    let dims = vec2<i32>(uniforms.viewport.xy);
    let lo = vec2<i32>(0);
    let hi = dims - vec2<i32>(1);
    let deltas = vec4<f32>(
        textureLoad(depthTexture, clamp(center_pixel + vec2<i32>(-1, 0), lo, hi), 0) - center_depth,
        textureLoad(depthTexture, clamp(center_pixel + vec2<i32>(1, 0), lo, hi), 0) - center_depth,
        textureLoad(depthTexture, clamp(center_pixel + vec2<i32>(0, -1), lo, hi), 0) - center_depth,
        textureLoad(depthTexture, clamp(center_pixel + vec2<i32>(0, 1), lo, hi), 0) - center_depth);
    return vec2<f32>(
        robust_axis_depth_slope(deltas.x, deltas.y),
        robust_axis_depth_slope(deltas.z, deltas.w));
}

fn planar_depth_residual(
    sample_pixel: vec2<i32>, center_pixel: vec2<i32>,
    center_depth: f32, depth_slopes: vec2<f32>) -> f32 {
    let offset = sample_pixel - center_pixel;
    let predicted_delta =
        depth_slopes.x * f32(abs(offset.x)) +
        depth_slopes.y * f32(abs(offset.y));
    let actual_delta =
        abs(textureLoad(depthTexture, sample_pixel, 0) - center_depth);
    return max(actual_delta - predicted_delta * 1.5, 0.0);
}

fn surface_weight(
    sample_pixel: vec2<i32>, center_pixel: vec2<i32>, center_depth: f32,
    center_normal: vec3<f32>, depth_slopes: vec2<f32>) -> f32 {
    let sample_normal = load_normal(sample_pixel);
    let depth_keep = exp(
        -planar_depth_residual(
            sample_pixel, center_pixel, center_depth, depth_slopes) /
            depth_sigma());
    let normal_keep = pow(max(dot(center_normal, sample_normal), 0.0), normal_sigma());
    return clamp(depth_keep * normal_keep, 0.0, 1.0);
}

struct SvgfLocalStatistics {
    variance: f32,
    minimum: vec3<f32>,
    maximum: vec3<f32>,
    radianceSupport: f32,
}

// Variance and neighborhood clipping use the same 3x3 footprint and the
// same surface tests. Accumulate both in one walk so every source, normal and
// depth sample is fetched only once per tap.
fn local_statistics(
    pixel: vec2<i32>, center_depth: f32,
    center_normal: vec3<f32>, depth_slopes: vec2<f32>) -> SvgfLocalStatistics {
    let dims = vec2<i32>(uniforms.viewport.xy);
    var sum = 0.0;
    var sum_sq = 0.0;
    var weight_sum = 0.0;
    var min_value = vec3<f32>(1.0e20);
    var max_value = vec3<f32>(0.0);
    var found = false;
    var support_sum = 0.0;
    let center_rgb = clamp_luminance(
        textureLoad(sourceTexture, pixel, 0).rgb);
    let center_luma = luminance(center_rgb);

    for (var y = -1; y <= 1; y = y + 1) {
        for (var x = -1; x <= 1; x = x + 1) {
            let sample_pixel = pixel + vec2<i32>(x, y);
            if (any(sample_pixel < vec2<i32>(0)) ||
                any(sample_pixel >= dims)) { continue; }
            let sample = textureLoad(sourceTexture, sample_pixel, 0);
            if (!spatial_sample_valid(sample.a)) { continue; }
            let w = surface_weight(
                sample_pixel, pixel, center_depth, center_normal,
                depth_slopes);
            let rgb = clamp_luminance(sample.rgb);
            let l = luminance(rgb);
            sum = sum + l * w;
            sum_sq = sum_sq + l * l * w;
            weight_sum = weight_sum + w;
            let support_threshold =
                max(0.01, max(center_luma, l) * 0.15);
            if (abs(l - center_luma) <= support_threshold) {
                support_sum = support_sum + w;
            }
            if (w > 1.0e-3) {
                min_value = min(min_value, rgb);
                max_value = max(max_value, rgb);
                found = true;
            }
        }
    }

    if (!found) {
        let center = clamp_luminance(textureLoad(sourceTexture, pixel, 0).rgb);
        min_value = center;
        max_value = center;
    }

    let inv_weight = 1.0 / max(weight_sum, 1.0e-5);
    let mean = sum * inv_weight;
    return SvgfLocalStatistics(
        max(sum_sq * inv_weight - mean * mean, 0.0),
        min_value,
        max_value,
        support_sum * inv_weight);
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let pixel = global_id.xy;
    if (pixel.x >= uniforms.viewport.x || pixel.y >= uniforms.viewport.y) {
        return;
    }

    let dims = vec2<i32>(uniforms.viewport.xy);
    let pixel_i = vec2<i32>(pixel);
    let center_sample = textureLoad(sourceTexture, pixel_i, 0);
    if (!enabled()) {
        textureStore(outputTexture, pixel_i, center_sample);
        return;
    }
    // Negative alpha is the accumulator's "reconstructed but valid" marker: a
    // pixel whose radiance was inferred from neighbours rather than measured.
    // It is legitimate filter input carrying no confidence at all, and must not
    // be confused with an uncovered pixel. The temporal pass already reads it
    // this way; reading it as zero here replaced exactly those pixels - the
    // freshly disoccluded ones - with black.
    let reconstructed = center_sample.a < -0.5;
    let unresolved = center_sample.a < -1.5;
    if (!reconstructed && center_sample.a <= 1.0e-5) {
        textureStore(outputTexture, pixel_i, vec4<f32>(0.0, 0.0, 0.0, 0.0));
        return;
    }
    // How settled this pixel is, as a fraction of the accumulator's window.
    // Reconstructed pixels are valid spatial targets but have no measurement
    // confidence of their own.
    let confidence = select(
        clamp(center_sample.a / max_history_frames(), 0.0, 1.0),
        0.0, reconstructed);

    let center_rgb = clamp_luminance(center_sample.rgb);
    let center_luma = luminance(center_rgb);
    let center_depth = textureLoad(depthTexture, pixel_i, 0);
    let center_normal = load_normal(pixel_i);
    let depth_slopes = planar_depth_slopes(pixel_i, center_depth);
    let statistics = local_statistics(
        pixel_i, center_depth, center_normal, depth_slopes);
    // A thin/dotted dark result on an otherwise compatible coplanar
    // neighbourhood is an unsupported stochastic sample, not a stable shadow
    // boundary. Coherent shadow regions retain support from their own side.
    let radiance_outlier = !reconstructed &&
        statistics.radianceSupport < 0.45;
    let normalized_variance =
        clamp(statistics.variance / variance_sigma(), 0.0, 8.0);
    let variance_response = 1.0 - exp(-normalized_variance);
    let filter_response = select(
        max(1.0 - confidence, variance_response * 0.5),
        1.0, radiance_outlier);
    let reconstruction_strength = select(
        spatial_strength(), 1.0, reconstructed);
    // A validated, settled measurement whose requested filter blend is below
    // 1/1024 needs no 25-tap walk. Keep its own sample count: borrowing spatial
    // radiance must not replace measurement confidence with a neighbour's.
    // Disocclusions and isolated visibility outliers always take the filter.
    if (!reconstructed && confidence >= 1.0 && !radiance_outlier &&
        reconstruction_strength * filter_response <= 1.0 / 1024.0) {
        textureStore(outputTexture, pixel_i, vec4<f32>(
            center_rgb, select(center_sample.a, 1.0, output_hit_mask())));
        return;
    }
    let variance_boost = 1.0 + normalized_variance;
    let luma_sigma = luminance_sigma() * variance_boost;
    let kernel = array<f32, 5>(1.0 / 16.0, 4.0 / 16.0, 6.0 / 16.0, 4.0 / 16.0, 1.0 / 16.0);
    // The graph supplies explicit powers-of-two scales. Expanding the stride a
    // second time for zero-confidence pixels skips over the measured receiver
    // inside small sparse windows and leaves a persistent black lattice.
    let stride = step_width();

    let center_valid = spatial_sample_valid(center_sample.a);
    let center_weight = select(
        0.0, kernel[2u] * kernel[2u],
        center_valid && !radiance_outlier);
    var sum = center_rgb * center_weight;
    // Alpha is an accumulated sample count everywhere except on a
    // reconstructed pixel, where it is the marker -1. Averaging the marker as
    // if it were a count is what let a mixed footprint of measured and
    // reconstructed neighbours land the result between -0.5 and 0: no longer
    // recognised as reconstructed, and small enough for the next iteration's
    // uncovered-pixel test to replace the pixel with black. Sum confidence
    // only, and re-apply the marker to the output below.
    var alpha_sum = max(center_sample.a, 0.0) * center_weight;
    var weight_sum = center_weight;

    for (var y = -2; y <= 2; y = y + 1) {
        for (var x = -2; x <= 2; x = x + 1) {
            if (x == 0 && y == 0) {
                continue;
            }

            let sample_pixel = pixel_i + vec2<i32>(x * stride, y * stride);
            // A viewport edge is not a repeated surface sample. Clamping every
            // outside kernel tap onto the last texel overweights it by up to an
            // entire half-kernel and turns camera rotation into edge streaks.
            if (any(sample_pixel < vec2<i32>(0)) ||
                any(sample_pixel >= dims)) { continue; }
            let sample = textureLoad(sourceTexture, sample_pixel, 0);
            if (!spatial_sample_valid(sample.a)) { continue; }
            let sample_rgb = clamp_luminance(sample.rgb);
            // RGB is a zero sentinel while alpha is -2; it is not a radiance
            // estimate. Using that zero as the bilateral guide preferentially
            // selected shadowed taps, after which successive A-trous scales
            // spread them into long dark rays. The first reconstruction is
            // geometry-guided only. Once alpha becomes -1, its reconstructed
            // RGB is a legitimate luminance guide for the remaining passes.
            let luma_difference =
                abs(luminance(sample_rgb) - center_luma);
            let luma_weight = select(
                exp(-luma_difference / luma_sigma),
                1.0, unresolved || radiance_outlier);
            let edge_weight = surface_weight(
                sample_pixel, pixel_i, center_depth, center_normal,
                depth_slopes);
            let history_weight = mix(0.35, 1.0, clamp(sample.a / max_history_frames(), 0.0, 1.0));
            let kernel_weight = kernel[u32(x + 2)] * kernel[u32(y + 2)];
            let weight =
                kernel_weight * edge_weight * luma_weight * history_weight;
            sum = sum + sample_rgb * weight;
            alpha_sum = alpha_sum + max(sample.a, 0.0) * weight;
            weight_sum = weight_sum + weight;
        }
    }

    let has_estimate = weight_sum > 1.0e-5;
    let filtered = sum / max(weight_sum, 1.0e-5);
    let bounds_slack = max((statistics.maximum - statistics.minimum) * 0.35, max(abs(center_rgb) * vec3<f32>(0.10), vec3<f32>(0.01)));
    let clamped_filtered = select(
        clamp(filtered, max(vec3<f32>(0.0), statistics.minimum - bounds_slack), statistics.maximum + bounds_slack),
        max(filtered, vec3<f32>(0.0)), reconstructed);
    // Blur in proportion to how little this pixel knows.
    //
    // A constant strength spends the same 5x5 blur on a pixel with a full
    // accumulation window as on one holding a single sample. The single sample
    // needs it; the settled pixel is already the average of a whole window, so
    // filtering it again every frame only removes detail the accumulator had
    // resolved - which is the image growing progressively softer the longer the
    // camera holds still. Confidence varies smoothly with the accumulated
    // count, so the strength does too and no seam appears between neighbours.
    // Sample count alone cannot prove convergence: a biased binary visibility
    // sequence can reach the history cap while remaining an isolated outlier.
    // Keep a bounded variance-driven response after temporal confidence fills;
    // bilateral geometry and luminance weights still preserve real edges.
    let output_rgb = mix(
        center_rgb, clamped_filtered,
        reconstruction_strength * clamp(filter_response, 0.0, 1.0));
    // Filtering borrows a neighbour's radiance; it does not give this pixel a
    // measurement of its own. A reconstructed centre stays reconstructed, so
    // the next iteration reads the marker rather than a near-zero count.
    let reconstructed_alpha = select(-2.0, -1.0, has_estimate);
    let filtered_alpha = select(
        alpha_sum / max(weight_sum, 1.0e-5), reconstructed_alpha,
        reconstructed);
    let output_alpha = select(filtered_alpha, 1.0, output_hit_mask());
    textureStore(
        outputTexture, pixel_i, vec4<f32>(output_rgb, output_alpha));
}
