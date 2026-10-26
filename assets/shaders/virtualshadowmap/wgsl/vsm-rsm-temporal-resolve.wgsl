struct RsmTemporalResolveUniforms {
    viewport: vec2<u32>,
    currentJitter: vec2<f32>,
    previousJitter: vec2<f32>,
    enabled: u32,
    historyValid: u32,
    debugMode: u32,
    _padding0: u32,
    historyWeight: f32,
    disocclusionWeight: f32,
    depthSigma: f32,
    normalSigma: f32,
    validRatioMin: f32,
    maxHistoryFrames: f32,
    fireflyClamp: f32,
    _padding1: vec2<f32>,
}

@group(0) @binding(0) var<uniform> uniforms: RsmTemporalResolveUniforms;
@group(0) @binding(1) var linearSampler: sampler;
@group(0) @binding(2) var currentTexture: texture_2d<f32>;
@group(0) @binding(3) var historyTexture: texture_2d<f32>;
@group(0) @binding(4) var motionVectorTexture: texture_2d<f32>;
@group(0) @binding(5) var disocclusionMaskTexture: texture_2d<f32>;
@group(0) @binding(6) var depthTexture: texture_depth_2d;
@group(0) @binding(7) var normalTexture: texture_2d<f32>;
@group(0) @binding(8) var historyDepthTexture: texture_depth_2d;
@group(0) @binding(9) var historyNormalTexture: texture_2d<f32>;
@group(0) @binding(10) var outputTexture: texture_storage_2d<rgba16float, write>;

fn texel_uv(pixel: vec2<u32>, dims: vec2<u32>) -> vec2<f32> {
    return (vec2<f32>(pixel) + vec2<f32>(0.5)) / vec2<f32>(max(dims, vec2<u32>(1u)));
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

fn load_disocclusion(pixel: vec2<u32>) -> f32 {
    let dims = textureDimensions(disocclusionMaskTexture);
    let uv = texel_uv(pixel, uniforms.viewport);
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

fn clamp_luminance(value: vec3<f32>) -> vec3<f32> {
    let max_luminance = max(uniforms.fireflyClamp, 1.0e-4);
    let luminance = max(dot(value, vec3<f32>(0.2126, 0.7152, 0.0722)), 0.0);
    let scale = min(1.0, max_luminance / max(luminance, 1.0e-4));
    return value * scale;
}

fn current_sample_and_neighborhood(pixel: vec2<u32>, center_depth: f32, center_normal: vec3<f32>, min_out: ptr<function, vec3<f32>>, max_out: ptr<function, vec3<f32>>) -> vec4<f32> {
    let dims = vec2<i32>(uniforms.viewport);
    let center_i = vec2<i32>(pixel);
    let center_sample = textureLoad(currentTexture, center_i, 0);
    let center_value = vec4<f32>(clamp_luminance(center_sample.rgb), center_sample.a);
    var neighborhood_min = center_value.rgb;
    var neighborhood_max = center_value.rgb;

    for (var y = -1; y <= 1; y = y + 1) {
        for (var x = -1; x <= 1; x = x + 1) {
            let sample_pixel = clamp(center_i + vec2<i32>(x, y), vec2<i32>(0), dims - vec2<i32>(1));
            var sample_value = textureLoad(currentTexture, sample_pixel, 0);
            sample_value = vec4<f32>(clamp_luminance(sample_value.rgb), sample_value.a);
            let sample_depth = textureLoad(depthTexture, sample_pixel, 0);
            let sample_normal = load_normal(sample_pixel);
            let depth_weight = exp(-abs(sample_depth - center_depth) / max(uniforms.depthSigma, 1.0e-6));
            let normal_weight = pow(max(dot(center_normal, sample_normal), 0.0), max(uniforms.normalSigma, 1.0e-4));
            let payload_weight = clamp(sample_value.a / max(uniforms.validRatioMin, 1.0e-4), 0.0, 1.0);
            let weight = depth_weight * normal_weight * payload_weight;
            if (weight > 1.0e-3) {
                neighborhood_min = min(neighborhood_min, sample_value.rgb);
                neighborhood_max = max(neighborhood_max, sample_value.rgb);
            }
        }
    }

    (*min_out) = neighborhood_min;
    (*max_out) = neighborhood_max;
    return center_value;
}

fn clip_history(history_rgb: vec3<f32>, current_rgb: vec3<f32>, neighborhood_min: vec3<f32>, neighborhood_max: vec3<f32>) -> vec3<f32> {
    let neighborhood_span = max(neighborhood_max - neighborhood_min, vec3<f32>(0.0));
    let radiance_slack = max(abs(current_rgb) * vec3<f32>(0.2), vec3<f32>(0.01));
    let clip_extent = neighborhood_span * 0.75 + radiance_slack;
    let loose_min = max(vec3<f32>(0.0), neighborhood_min - clip_extent);
    let loose_max = neighborhood_max + clip_extent;
    return clamp(history_rgb, loose_min, loose_max);
}

fn neighborhood_relative_variance(pixel: vec2<u32>, center_depth: f32, center_normal: vec3<f32>) -> f32 {
    let dims = vec2<i32>(uniforms.viewport);
    let center_i = vec2<i32>(pixel);
    var sum = 0.0;
    var sum_sq = 0.0;
    var weight_sum = 0.0;
    for (var y = -1; y <= 1; y = y + 1) {
        for (var x = -1; x <= 1; x = x + 1) {
            let sample_pixel = clamp(center_i + vec2<i32>(x, y), vec2<i32>(0), dims - vec2<i32>(1));
            let sample_value = textureLoad(currentTexture, sample_pixel, 0);
            let sample_depth = textureLoad(depthTexture, sample_pixel, 0);
            let sample_normal = load_normal(sample_pixel);
            let depth_weight = exp(-abs(sample_depth - center_depth) / max(uniforms.depthSigma, 1.0e-6));
            let normal_weight = pow(max(dot(center_normal, sample_normal), 0.0), max(uniforms.normalSigma, 1.0e-4));
            let payload_weight = clamp(sample_value.a / max(uniforms.validRatioMin, 1.0e-4), 0.0, 1.0);
            let weight = depth_weight * normal_weight * payload_weight;
            let luma = dot(clamp_luminance(sample_value.rgb), vec3<f32>(0.2126, 0.7152, 0.0722));
            sum = sum + luma * weight;
            sum_sq = sum_sq + luma * luma * weight;
            weight_sum = weight_sum + weight;
        }
    }
    let mean = sum / max(weight_sum, 1.0e-6);
    let variance = max(sum_sq / max(weight_sum, 1.0e-6) - mean * mean, 0.0);
    return clamp(variance / max(mean * mean, 0.01), 0.0, 8.0);
}

fn history_pixel_surface_keep(history_pixel: vec2<i32>, center_depth: f32, center_normal: vec3<f32>) -> f32 {
    let history_depth = textureLoad(historyDepthTexture, history_pixel, 0);
    let history_normal = load_history_normal(history_pixel);
    let depth_keep = exp(-abs(history_depth - center_depth) / max(uniforms.depthSigma, 1.0e-6));
    let normal_keep = pow(max(dot(center_normal, history_normal), 0.0), max(uniforms.normalSigma, 1.0e-4));
    return clamp(depth_keep * normal_keep, 0.0, 1.0);
}

fn load_reprojected_history(history_uv: vec2<f32>, center_depth: f32, center_normal: vec3<f32>) -> vec4<f32> {
    let dims = vec2<i32>(uniforms.viewport);
    let history_coord = history_uv * vec2<f32>(uniforms.viewport) - vec2<f32>(0.5);
    let base = vec2<i32>(floor(history_coord));
    let frac = fract(history_coord);
    var radiance_sum = vec3<f32>(0.0);
    var count_sum = 0.0;

    for (var y = 0; y <= 1; y = y + 1) {
        for (var x = 0; x <= 1; x = x + 1) {
            let offset = vec2<i32>(x, y);
            let history_pixel = clamp(base + offset, vec2<i32>(0), dims - vec2<i32>(1));
            let wx = select(1.0 - frac.x, frac.x, x == 1);
            let wy = select(1.0 - frac.y, frac.y, y == 1);
            let bilinear_weight = wx * wy;
            let surface_keep = history_pixel_surface_keep(history_pixel, center_depth, center_normal);
            let history_sample = textureLoad(historyTexture, history_pixel, 0);
            let history_count = clamp(history_sample.a, 0.0, max(uniforms.maxHistoryFrames, 1.0));
            let tap_weight = bilinear_weight * surface_keep * history_count;
            radiance_sum = radiance_sum + clamp_luminance(history_sample.rgb) * tap_weight;
            count_sum = count_sum + tap_weight;
        }
    }

    if (count_sum <= 1.0e-5) {
        return vec4<f32>(0.0, 0.0, 0.0, 0.0);
    }
    return vec4<f32>(radiance_sum / count_sum, count_sum);
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let pixel = global_id.xy;
    if (pixel.x >= uniforms.viewport.x || pixel.y >= uniforms.viewport.y) {
        return;
    }

    let pixel_i = vec2<i32>(pixel);
    if (uniforms.enabled == 0u || uniforms.debugMode != 0u) {
        textureStore(outputTexture, pixel_i, textureLoad(currentTexture, pixel_i, 0));
        return;
    }

    let center_depth = textureLoad(depthTexture, pixel_i, 0);
    let center_normal = load_normal(pixel_i);
    var neighborhood_min = vec3<f32>(0.0);
    var neighborhood_max = vec3<f32>(0.0);
    let current = current_sample_and_neighborhood(pixel, center_depth, center_normal, &neighborhood_min, &neighborhood_max);
    let current_confidence = clamp(current.a / max(uniforms.validRatioMin, 1.0e-4), 0.0, 1.0);
    let current_relative_variance = neighborhood_relative_variance(pixel, center_depth, center_normal);
    if (current.a <= 1.0e-5) {
        textureStore(outputTexture, pixel_i, vec4<f32>(0.0, 0.0, 0.0, 0.0));
        return;
    }
    var output_value = vec4<f32>(current.rgb, max(current_confidence, 0.05));
    let disocclusion = load_disocclusion(pixel);
    let uv = texel_uv(pixel, uniforms.viewport);
    let velocity = textureLoad(motionVectorTexture, pixel_i, 0).xy;
    let viewport_f = vec2<f32>(max(uniforms.viewport, vec2<u32>(1u)));
    let jitter_velocity = (uniforms.currentJitter - uniforms.previousJitter) / viewport_f;
    let history_uv = uv - (velocity - jitter_velocity);
    let history_offscreen =
        history_uv.x < 0.0 || history_uv.y < 0.0 ||
        history_uv.x > 1.0 || history_uv.y > 1.0;

    if (uniforms.historyValid != 0u && !history_offscreen) {
        let history = load_reprojected_history(history_uv, center_depth, center_normal);
        let clipped_history_rgb = clip_history(history.rgb, current.rgb, neighborhood_min, neighborhood_max);
        let history_keep = (1.0 - clamp(disocclusion * uniforms.disocclusionWeight, 0.0, 0.9));
        let max_history_frames = max(uniforms.maxHistoryFrames, 1.0);
        let history_count = clamp(history.a, 0.0, max_history_frames) * history_keep;
        let current_weight = max(current_confidence, 1.0 / max_history_frames);
        let running_average_weight = history_count / max(history_count + current_weight, 1.0e-5);
        let variance_keep = exp(-current_relative_variance * 0.75);
        let history_weight = min(running_average_weight, clamp(uniforms.historyWeight, 0.0, 0.99)) * variance_keep;
        output_value = vec4<f32>(mix(current.rgb, clipped_history_rgb, history_weight), 0.0);
        output_value.a = min(history_count + current_weight, max_history_frames);
    }

    textureStore(outputTexture, pixel_i, output_value);
}
