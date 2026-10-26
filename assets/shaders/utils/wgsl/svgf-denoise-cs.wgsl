struct SvgfDenoiseUniforms {
    viewport: vec4<u32>,
    jitter: vec4<f32>,
    params0: vec4<f32>,
    params1: vec4<f32>,
    params2: vec4<f32>,
    params3: vec4<f32>,
}

@group(0) @binding(0) var<uniform> uniforms: SvgfDenoiseUniforms;
@group(0) @binding(1) var currentTexture: texture_2d<f32>;
@group(0) @binding(2) var historyTexture: texture_2d<f32>;
@group(0) @binding(3) var motionVectorTexture: texture_2d<f32>;
@group(0) @binding(4) var disocclusionMaskTexture: texture_2d<f32>;
@group(0) @binding(5) var depthTexture: texture_depth_2d;
@group(0) @binding(6) var normalTexture: texture_2d<f32>;
@group(0) @binding(7) var historyDepthTexture: texture_depth_2d;
@group(0) @binding(8) var historyNormalTexture: texture_2d<f32>;
@group(0) @binding(9) var currentInstanceTexture: texture_2d<u32>;
@group(0) @binding(10) var currentVoxelTexture: texture_2d<u32>;
@group(0) @binding(11) var historyInstanceTexture: texture_2d<u32>;
@group(0) @binding(12) var historyVoxelTexture: texture_2d<u32>;
@group(0) @binding(13) var outputTexture: texture_storage_2d<rgba16float, write>;

fn enabled() -> bool {
    return uniforms.viewport.z != 0u;
}

fn history_valid() -> bool {
    return uniforms.viewport.w != 0u;
}

fn history_weight_limit() -> f32 {
    return clamp(uniforms.params0.x, 0.0, 0.99);
}

fn depth_sigma() -> f32 {
    return max(uniforms.params0.y, 1.0e-6);
}

fn normal_sigma() -> f32 {
    return max(uniforms.params0.z, 1.0e-4);
}

fn luminance_sigma() -> f32 {
    return max(uniforms.params0.w, 1.0e-4);
}

fn valid_ratio_min() -> f32 {
    return max(uniforms.params1.x, 1.0e-4);
}

fn max_history_frames() -> f32 {
    return max(uniforms.params1.y, 1.0);
}

fn firefly_clamp() -> f32 {
    return max(uniforms.params1.z, 1.0e-4);
}

fn spatial_sigma() -> f32 {
    return max(uniforms.params1.w, 1.0e-4);
}

fn spatial_radius() -> i32 {
    return i32(clamp(uniforms.params2.x, 1.0, 8.0));
}

fn spatial_strength() -> f32 {
    return clamp(uniforms.params2.y, 0.0, 1.0);
}

fn disocclusion_weight() -> f32 {
    return max(uniforms.params2.z, 0.0);
}

fn min_current_weight() -> f32 {
    return clamp(uniforms.params2.w, 0.01, 1.0);
}

fn history_clip_strength() -> f32 {
    return clamp(uniforms.params3.x, 0.0, 1.0);
}

// Half-width of the neighbourhood the history clip is built from. The lighting
// input is upsampled from a reduced-resolution ray grid, so a single outlier
// ray covers a whole divisor-sized pixel block. A fixed 3x3 window sits
// entirely inside such a block, which makes the block its own neighbourhood
// and lets the clip pass it through unchanged. The caller sets this to at
// least the ray-resolution divisor.
fn clamp_radius() -> i32 {
    return i32(clamp(uniforms.params3.y, 1.0, 4.0));
}

fn texel_uv(pixel: vec2<u32>) -> vec2<f32> {
    return (vec2<f32>(pixel) + vec2<f32>(0.5)) / vec2<f32>(max(uniforms.viewport.xy, vec2<u32>(1u)));
}

fn load_normal(pixel: vec2<i32>) -> vec3<f32> {
    let n = textureLoad(normalTexture, pixel, 0).xyz;
    let len_sq = dot(n, n);
    return select(vec3<f32>(0.0, 1.0, 0.0), n * inverseSqrt(len_sq), len_sq > 1.0e-8);
}

fn load_history_normal(pixel: vec2<i32>) -> vec3<f32> {
    let n = textureLoad(historyNormalTexture, pixel, 0).xyz;
    let len_sq = dot(n, n);
    return select(vec3<f32>(0.0, 1.0, 0.0), n * inverseSqrt(len_sq), len_sq > 1.0e-8);
}

fn clamp_luminance(value: vec3<f32>) -> vec3<f32> {
    let max_luminance = firefly_clamp();
    let luminance = max(dot(value, vec3<f32>(0.2126, 0.7152, 0.0722)), 0.0);
    return value * min(1.0, max_luminance / max(luminance, 1.0e-4));
}

fn luminance(value: vec3<f32>) -> f32 {
    return max(dot(value, vec3<f32>(0.2126, 0.7152, 0.0722)), 0.0);
}

fn load_disocclusion(pixel: vec2<u32>) -> f32 {
    let dims = textureDimensions(disocclusionMaskTexture);
    let uv = texel_uv(pixel);
    let center_pixel = clamp(vec2<i32>(floor(uv * vec2<f32>(dims))), vec2<i32>(0), vec2<i32>(dims) - vec2<i32>(1));
    var disocclusion = 0.0;
    for (var y = -1; y <= 1; y = y + 1) {
        for (var x = -1; x <= 1; x = x + 1) {
            let sample_pixel = clamp(center_pixel + vec2<i32>(x, y), vec2<i32>(0), vec2<i32>(dims) - vec2<i32>(1));
            disocclusion = max(disocclusion, textureLoad(disocclusionMaskTexture, sample_pixel, 0).x);
        }
    }
    return clamp(disocclusion, 0.0, 1.0);
}

fn surface_keep(sample_pixel: vec2<i32>, center_depth: f32, center_normal: vec3<f32>) -> f32 {
    let sample_depth = textureLoad(depthTexture, sample_pixel, 0);
    let sample_normal = load_normal(sample_pixel);
    let depth_keep = exp(-abs(sample_depth - center_depth) / depth_sigma());
    let normal_keep = pow(max(dot(center_normal, sample_normal), 0.0), normal_sigma());
    return clamp(depth_keep * normal_keep, 0.0, 1.0);
}

fn history_surface_keep(
    sample_pixel: vec2<i32>, center_depth: f32,
    center_normal: vec3<f32>, center_identity: vec2<u32>) -> f32 {
    let sample_depth = textureLoad(historyDepthTexture, sample_pixel, 0);
    let sample_identity = vec2<u32>(
        textureLoad(historyInstanceTexture, sample_pixel, 0).x,
        textureLoad(historyVoxelTexture, sample_pixel, 0).x);
    if (any(sample_identity != center_identity)) {
        return 0.0;
    }
    let center_background = center_depth <= 1.0e-7;
    let sample_background = sample_depth <= 1.0e-7;
    if (center_background != sample_background) {
        return 0.0;
    }
    let sample_normal = load_history_normal(sample_pixel);
    let normal_keep = pow(max(dot(center_normal, sample_normal), 0.0), normal_sigma());
    return clamp(normal_keep, 0.0, 1.0);
}

fn neighborhood_bounds(
    pixel: vec2<u32>, center_depth: f32, center_normal: vec3<f32>,
    min_out: ptr<function, vec3<f32>>,
    max_out: ptr<function, vec3<f32>>) {
    let dims = vec2<i32>(uniforms.viewport.xy);
    let center_i = vec2<i32>(pixel);
    let center_value = textureLoad(currentTexture, center_i, 0);
    var min_value = clamp_luminance(center_value.rgb);
    var max_value = min_value;
    let radius = clamp_radius();
    for (var y = -radius; y <= radius; y = y + 1) {
        for (var x = -radius; x <= radius; x = x + 1) {
            let sample_pixel = clamp(center_i + vec2<i32>(x, y), vec2<i32>(0), dims - vec2<i32>(1));
            let sample_value = textureLoad(currentTexture, sample_pixel, 0);
            let confidence = clamp(sample_value.a / valid_ratio_min(), 0.0, 1.0);
            let weight = surface_keep(sample_pixel, center_depth, center_normal) * confidence;
            let rgb = clamp_luminance(sample_value.rgb);
            if (weight > 1.0e-3) {
                min_value = min(min_value, rgb);
                max_value = max(max_value, rgb);
            }
        }
    }

    (*min_out) = min_value;
    (*max_out) = max_value;
}

fn clip_history(history_rgb: vec3<f32>, current_rgb: vec3<f32>, neighborhood_min: vec3<f32>, neighborhood_max: vec3<f32>) -> vec3<f32> {
    let span = max(neighborhood_max - neighborhood_min, vec3<f32>(0.0));
    let slack = max(abs(current_rgb) * vec3<f32>(0.20), vec3<f32>(0.01));
    let extent = span * 0.75 + slack;
    let clipped = clamp(
        history_rgb,
        max(vec3<f32>(0.0), neighborhood_min - extent),
        neighborhood_max + extent);
    return mix(history_rgb, clipped, history_clip_strength());
}

fn load_reprojected_history(
    history_uv: vec2<f32>, center_depth: f32,
    center_normal: vec3<f32>, center_identity: vec2<u32>) -> vec4<f32> {
    let dims = vec2<i32>(uniforms.viewport.xy);
    let history_coord = history_uv * vec2<f32>(uniforms.viewport.xy) - vec2<f32>(0.5);
    let base = vec2<i32>(floor(history_coord));
    let frac = fract(history_coord);
    var radiance_sum = vec3<f32>(0.0);
    var count_sum = 0.0;

    for (var y = 0; y <= 1; y = y + 1) {
        for (var x = 0; x <= 1; x = x + 1) {
            let offset = vec2<i32>(x, y);
            let sample_pixel = clamp(base + offset, vec2<i32>(0), dims - vec2<i32>(1));
            let wx = select(1.0 - frac.x, frac.x, x == 1);
            let wy = select(1.0 - frac.y, frac.y, y == 1);
            let sample = textureLoad(historyTexture, sample_pixel, 0);
            let count = clamp(sample.a, 0.0, max_history_frames());
            let weight = wx * wy * history_surface_keep(
                sample_pixel, center_depth, center_normal,
                center_identity) * count;
            radiance_sum = radiance_sum + clamp_luminance(sample.rgb) * weight;
            count_sum = count_sum + weight;
        }
    }

    if (count_sum <= 1.0e-5) {
        return vec4<f32>(0.0, 0.0, 0.0, 0.0);
    }
    return vec4<f32>(radiance_sum / count_sum, count_sum);
}

fn spatial_atrous_filter(pixel: vec2<u32>, center_depth: f32, center_normal: vec3<f32>, center_rgb: vec3<f32>) -> vec3<f32> {
    let dims = vec2<i32>(uniforms.viewport.xy);
    let center_i = vec2<i32>(pixel);
    let radius = spatial_radius();
    var sum = center_rgb;
    var weight_sum = 1.0;
    let center_luma = dot(center_rgb, vec3<f32>(0.2126, 0.7152, 0.0722));
    let kernel = array<f32, 5>(1.0 / 16.0, 4.0 / 16.0, 6.0 / 16.0, 4.0 / 16.0, 1.0 / 16.0);

    for (var y = -2; y <= 2; y = y + 1) {
        for (var x = -2; x <= 2; x = x + 1) {
            if (x == 0 && y == 0) {
                continue;
            }
            let sample_pixel = clamp(center_i + vec2<i32>(x * radius, y * radius), vec2<i32>(0), dims - vec2<i32>(1));
            let sample_value = textureLoad(currentTexture, sample_pixel, 0);
            let sample_rgb = clamp_luminance(sample_value.rgb);
            let sample_luma = dot(sample_rgb, vec3<f32>(0.2126, 0.7152, 0.0722));
            let confidence = clamp(sample_value.a / valid_ratio_min(), 0.0, 1.0);
            let luminance_weight = exp(-abs(sample_luma - center_luma) / luminance_sigma());
            let filter_weight = kernel[u32(abs(x))] * kernel[u32(abs(y))] * surface_keep(sample_pixel, center_depth, center_normal) * luminance_weight * confidence;
            sum = sum + sample_rgb * filter_weight;
            weight_sum = weight_sum + filter_weight;
        }
    }
    return sum / max(weight_sum, 1.0e-5);
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let pixel = global_id.xy;
    if (pixel.x >= uniforms.viewport.x || pixel.y >= uniforms.viewport.y) {
        return;
    }

    let pixel_i = vec2<i32>(pixel);
    let current_sample = textureLoad(currentTexture, pixel_i, 0);
    // Per-pixel ReSTIR feeds this pass a full-resolution light field that
    // already owns temporal accumulation and confidence in alpha. A zero
    // history limit requests a true temporal bypass: preserve that signal
    // verbatim for the optional a-trous stage instead of applying another
    // firefly clamp or rewriting its sample count.
    if (enabled() && history_weight_limit() <= 1.0e-5) {
        textureStore(
            outputTexture, pixel_i,
            vec4<f32>(max(current_sample.rgb, vec3<f32>(0.0)),
                current_sample.a));
        return;
    }
    let current_rgb = clamp_luminance(current_sample.rgb);
    // The light field uses negative alpha for a geometrically reconstructed
    // sample that has no direct measurement yet. It is valid filter input, but
    // deliberately carries only the minimum temporal weight so the first
    // traced sample replaces it quickly.
    let reconstructed_sample = current_sample.a < -0.5;
    let current_confidence = select(
        clamp(current_sample.a / valid_ratio_min(), 0.0, 1.0),
        min_current_weight(), reconstructed_sample);
    if (!enabled()) {
        textureStore(outputTexture, pixel_i, vec4<f32>(current_rgb, current_sample.a));
        return;
    }
    if (abs(current_sample.a) <= 1.0e-5) {
        textureStore(outputTexture, pixel_i, vec4<f32>(0.0, 0.0, 0.0, 0.0));
        return;
    }

    let center_depth = textureLoad(depthTexture, pixel_i, 0);
    let center_normal = load_normal(pixel_i);
    let center_identity = vec2<u32>(
        textureLoad(currentInstanceTexture, pixel_i, 0).x,
        textureLoad(currentVoxelTexture, pixel_i, 0).x);
    var neighborhood_min = current_rgb;
    var neighborhood_max = current_rgb;
    // Neighborhood clipping costs nine additional source/surface reads and
    // biases alternating one-sample lighting. Skip it completely when the
    // user-controlled clipping strength is zero.
    if (history_clip_strength() > 0.0) {
        neighborhood_bounds(
            pixel, center_depth, center_normal,
            &neighborhood_min, &neighborhood_max);
    }

    var temporal_rgb = current_rgb;
    var history_count = 0.0;
    let disocclusion = load_disocclusion(pixel);
    let uv = texel_uv(pixel);
    let velocity = textureLoad(motionVectorTexture, pixel_i, 0).xy;
    // G-buffer motion is current UV minus previous UV and is generated from
    // the jittered current and previous clip transforms. Applying the jitter
    // delta again shifts history by a second subpixel offset.
    let history_uv = uv - velocity;
    let history_offscreen = history_uv.x < 0.0 || history_uv.y < 0.0 || history_uv.x > 1.0 || history_uv.y > 1.0;
    if (history_valid() && !history_offscreen) {
        let history = load_reprojected_history(
            history_uv, center_depth, center_normal, center_identity);
        let clipped_history = clip_history(history.rgb, current_rgb, neighborhood_min, neighborhood_max);
        // Current-frame Monte Carlo variance is evidence that more temporal
        // accumulation is needed, not that valid history should be discarded.
        // Allow a full disocclusion to reset accumulation. Capping the
        // rejection short of 1.0 kept a tenth of the accumulated sample
        // count, which still outweighs the single current sample and leaks
        // the previous surface's lighting for several frames.
        let history_keep =
            1.0 - clamp(disocclusion * disocclusion_weight(), 0.0, 1.0);
        history_count = clamp(history.a, 0.0, max_history_frames()) * history_keep;
        let current_weight = max(current_confidence, min_current_weight());
        let history_weight = min(history_count / max(history_count + current_weight, 1.0e-5), history_weight_limit());
        temporal_rgb = mix(current_rgb, clipped_history, history_weight);
        history_count = min(history_count + current_weight, max_history_frames());
    } else {
        history_count = max(current_confidence, min_current_weight());
    }

    textureStore(outputTexture, pixel_i, vec4<f32>(temporal_rgb, history_count));
}
