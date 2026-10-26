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

fn load_normal(pixel: vec2<i32>) -> vec3<f32> {
    let n = textureLoad(normalTexture, pixel, 0).xyz;
    let len_sq = dot(n, n);
    return select(vec3<f32>(0.0, 1.0, 0.0), n * inverseSqrt(len_sq), len_sq > 1.0e-8);
}

fn luminance(value: vec3<f32>) -> f32 {
    return max(dot(value, vec3<f32>(0.2126, 0.7152, 0.0722)), 0.0);
}

fn clamp_luminance(value: vec3<f32>) -> vec3<f32> {
    let luma = luminance(value);
    return value * min(1.0, firefly_clamp() / max(luma, 1.0e-4));
}

fn surface_weight(sample_pixel: vec2<i32>, center_depth: f32, center_normal: vec3<f32>) -> f32 {
    let sample_depth = textureLoad(depthTexture, sample_pixel, 0);
    let sample_normal = load_normal(sample_pixel);
    let depth_keep = exp(-abs(sample_depth - center_depth) / depth_sigma());
    let normal_keep = pow(max(dot(center_normal, sample_normal), 0.0), normal_sigma());
    return clamp(depth_keep * normal_keep, 0.0, 1.0);
}

fn local_variance(pixel: vec2<i32>, center_depth: f32, center_normal: vec3<f32>) -> f32 {
    let dims = vec2<i32>(uniforms.viewport.xy);
    var sum = 0.0;
    var sum_sq = 0.0;
    var weight_sum = 0.0;

    for (var y = -1; y <= 1; y = y + 1) {
        for (var x = -1; x <= 1; x = x + 1) {
            let sample_pixel = clamp(pixel + vec2<i32>(x, y), vec2<i32>(0), dims - vec2<i32>(1));
            let sample = textureLoad(sourceTexture, sample_pixel, 0);
            let w = surface_weight(sample_pixel, center_depth, center_normal);
            let l = luminance(clamp_luminance(sample.rgb));
            sum = sum + l * w;
            sum_sq = sum_sq + l * l * w;
            weight_sum = weight_sum + w;
        }
    }

    let inv_weight = 1.0 / max(weight_sum, 1.0e-5);
    let mean = sum * inv_weight;
    return max(sum_sq * inv_weight - mean * mean, 0.0);
}

fn neighborhood_bounds(pixel: vec2<i32>, center_depth: f32, center_normal: vec3<f32>, min_out: ptr<function, vec3<f32>>, max_out: ptr<function, vec3<f32>>) {
    let dims = vec2<i32>(uniforms.viewport.xy);
    var min_value = vec3<f32>(1.0e20);
    var max_value = vec3<f32>(0.0);
    var found = false;

    for (var y = -1; y <= 1; y = y + 1) {
        for (var x = -1; x <= 1; x = x + 1) {
            let sample_pixel = clamp(pixel + vec2<i32>(x, y), vec2<i32>(0), dims - vec2<i32>(1));
            let sample = textureLoad(sourceTexture, sample_pixel, 0);
            let w = surface_weight(sample_pixel, center_depth, center_normal);
            if (sample.a > 1.0e-5 && w > 1.0e-3) {
                let rgb = clamp_luminance(sample.rgb);
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

    (*min_out) = min_value;
    (*max_out) = max_value;
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
    if (center_sample.a <= 1.0e-5) {
        textureStore(outputTexture, pixel_i, vec4<f32>(0.0, 0.0, 0.0, 0.0));
        return;
    }

    let center_rgb = clamp_luminance(center_sample.rgb);
    let center_luma = luminance(center_rgb);
    let center_depth = textureLoad(depthTexture, pixel_i, 0);
    let center_normal = load_normal(pixel_i);
    let history_factor = clamp(center_sample.a / max_history_frames(), 0.0, 1.0);
    let variance = local_variance(pixel_i, center_depth, center_normal);
    var neighborhood_min = center_rgb;
    var neighborhood_max = center_rgb;
    neighborhood_bounds(pixel_i, center_depth, center_normal, &neighborhood_min, &neighborhood_max);
    let variance_boost = 1.0 + clamp(variance / variance_sigma(), 0.0, 8.0);
    let luma_sigma = luminance_sigma() * variance_boost;
    let confidence_boost = mix(2.5, 1.0, history_factor);
    let kernel = array<f32, 5>(1.0 / 16.0, 4.0 / 16.0, 6.0 / 16.0, 4.0 / 16.0, 1.0 / 16.0);
    let stride = step_width();

    var sum = center_rgb * kernel[2u] * kernel[2u];
    var alpha_sum = center_sample.a * kernel[2u] * kernel[2u];
    var weight_sum = kernel[2u] * kernel[2u];

    for (var y = -2; y <= 2; y = y + 1) {
        for (var x = -2; x <= 2; x = x + 1) {
            if (x == 0 && y == 0) {
                continue;
            }

            let sample_pixel = clamp(pixel_i + vec2<i32>(x * stride, y * stride), vec2<i32>(0), dims - vec2<i32>(1));
            let sample = textureLoad(sourceTexture, sample_pixel, 0);
            let sample_rgb = clamp_luminance(sample.rgb);
            let luma_weight = exp(-abs(luminance(sample_rgb) - center_luma) / luma_sigma);
            let edge_weight = surface_weight(sample_pixel, center_depth, center_normal);
            let history_weight = mix(0.35, 1.0, clamp(sample.a / max_history_frames(), 0.0, 1.0));
            let kernel_weight = kernel[u32(x + 2)] * kernel[u32(y + 2)];
            let weight = kernel_weight * edge_weight * luma_weight * history_weight * confidence_boost;
            sum = sum + sample_rgb * weight;
            alpha_sum = alpha_sum + sample.a * weight;
            weight_sum = weight_sum + weight;
        }
    }

    let filtered = sum / max(weight_sum, 1.0e-5);
    let bounds_slack = max((neighborhood_max - neighborhood_min) * 0.35, max(abs(center_rgb) * vec3<f32>(0.10), vec3<f32>(0.01)));
    let clamped_filtered = clamp(filtered, max(vec3<f32>(0.0), neighborhood_min - bounds_slack), neighborhood_max + bounds_slack);
    let output_rgb = mix(center_rgb, clamped_filtered, spatial_strength());
    textureStore(outputTexture, pixel_i, vec4<f32>(output_rgb, alpha_sum / max(weight_sum, 1.0e-5)));
}
