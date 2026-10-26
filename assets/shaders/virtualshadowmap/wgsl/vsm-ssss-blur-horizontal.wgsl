#include "vsm-common.wgsl"
#include "vsm-ssss-common.wgsl"

struct CameraState {
    inverseView: mat4x4<f32>,
    inverseProjection: mat4x4<f32>,
    inverseViewProjection: mat4x4<f32>,
    viewProjection: mat4x4<f32>,
    cameraPosition: vec4<f32>,
    reverseZ: u32,
    _padding: vec3<u32>,
}

@group(0) @binding(0) var<uniform> uniforms: ScreenSpaceSoftShadowUniforms;
@group(0) @binding(1) var depthTexture: texture_depth_2d;
@group(0) @binding(2) var normalTexture: texture_2d<f32>;
@group(0) @binding(3) var sourceTexture: texture_2d_array<f32>;
@group(0) @binding(4) var penumbraTexture: texture_2d_array<f32>;
@group(0) @binding(5) var pingTexture: texture_storage_2d_array<rgba16float, write>;
@group(0) @binding(6) var<storage, read> cameraState: CameraState;

fn ssss_reconstruct_view_position(pixel_coord: vec2<i32>, depth: f32) -> vec3<f32> {
    let uv = (vec2<f32>(pixel_coord) + vec2<f32>(0.5, 0.5)) / vec2<f32>(max(uniforms.inputViewport, vec2<u32>(1u, 1u)));
    return vsm_reconstruct_view_position_from_uv_depth(uv, depth, cameraState.inverseProjection);
}

fn ssss_geometry_sample_is_compatible(
    center_depth: f32,
    center_view_position: vec3<f32>,
    center_encoded_normal: vec3<f32>,
    sample_depth: f32,
    sample_view_position: vec3<f32>,
    sample_encoded_normal: vec3<f32>
) -> bool {
    if (!ssss_depths_are_compatible(center_depth, sample_depth)) {
        return false;
    }
    if (!ssss_normals_are_compatible(center_encoded_normal, sample_encoded_normal)) {
        return false;
    }

    let center_distance = max(length(center_view_position), 1e-3);
    let metric_delta = length(sample_view_position - center_view_position);
    let metric_limit = max(0.025 * center_distance, 0.05);
    if (metric_delta > metric_limit) {
        return false;
    }

    if (ssss_normal_is_valid(center_encoded_normal)) {
        let center_normal = ssss_decode_normal(center_encoded_normal);
        let plane_delta = abs(dot(sample_view_position - center_view_position, center_normal));
        if (plane_delta > max(0.01 * center_distance, 0.02)) {
            return false;
        }
    }

    return true;
}

fn ssss_sample_is_foreground_discontinuity(
    center_depth: f32,
    center_view_position: vec3<f32>,
    center_encoded_normal: vec3<f32>,
    sample_input: vec2<i32>
) -> bool {
    let input_dims_i = vec2<i32>(uniforms.inputViewport);
    if (sample_input.x < 0 || sample_input.y < 0 || sample_input.x >= input_dims_i.x || sample_input.y >= input_dims_i.y) {
        return false;
    }

    let sample_depth = textureLoad(depthTexture, sample_input, 0);
    if (center_depth <= 0.0 || sample_depth <= 0.0) {
        return false;
    }

    let sample_view_position = ssss_reconstruct_view_position(sample_input, sample_depth);
    let center_distance = length(center_view_position);
    let sample_distance = length(sample_view_position);
    if ((center_distance - sample_distance) <= max(0.0015 * center_distance, 0.005)) {
        return false;
    }

    let sample_normal = textureLoad(normalTexture, sample_input, 0).xyz;
    return !ssss_geometry_sample_is_compatible(
        center_depth,
        center_view_position,
        center_encoded_normal,
        sample_depth,
        sample_view_position,
        sample_normal);
}

fn ssss_is_near_foreground_discontinuity(
    center_input: vec2<i32>,
    center_depth: f32,
    center_view_position: vec3<f32>,
    center_encoded_normal: vec3<f32>
) -> bool {
    let radius = i32(max(1u, min(uniforms.resolutionScale / 2u, 2u)));
    return
        ssss_sample_is_foreground_discontinuity(center_depth, center_view_position, center_encoded_normal, center_input + vec2<i32>( radius,  0)) ||
        ssss_sample_is_foreground_discontinuity(center_depth, center_view_position, center_encoded_normal, center_input + vec2<i32>(-radius,  0)) ||
        ssss_sample_is_foreground_discontinuity(center_depth, center_view_position, center_encoded_normal, center_input + vec2<i32>( 0,  radius)) ||
        ssss_sample_is_foreground_discontinuity(center_depth, center_view_position, center_encoded_normal, center_input + vec2<i32>( 0, -radius));
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let dims = uniforms.viewport;
    if (!ssss_in_bounds(global_id.xy, dims) || global_id.z >= max(uniforms.layerCount, 1u)) {
        return;
    }

    let pixel = vec2<i32>(global_id.xy);
    let layer = i32(global_id.z);
    let center_input = ssss_output_to_input_pixel(global_id.xy);
    let center_depth = textureLoad(depthTexture, center_input, 0);
    let center_normal = textureLoad(normalTexture, center_input, 0).xyz;
    let center_view_position = ssss_reconstruct_view_position(center_input, center_depth);
    let penumbra = textureLoad(penumbraTexture, center_input, layer, 0).r;
    let fallback = textureLoad(sourceTexture, center_input, layer, 0);
    var filter_radius = max(0.0, penumbra * f32(uniforms.kernelSize));
    if (fallback.r < 0.5 && filter_radius > 1.0 && ssss_is_near_foreground_discontinuity(center_input, center_depth, center_view_position, center_normal)) {
        filter_radius = 1.0;
    }
    let kernel_radius = i32(ceil(filter_radius + 1.0));

    var sum = vec4<f32>(0.0);
    var weight_sum = 0.0;
    let half_kernel = i32(uniforms.kernelSize / 2u);
    for (var offset = -half_kernel; offset <= half_kernel; offset = offset + 1) {
        if (abs(offset) > kernel_radius) {
            continue;
        }
        let sample_pixel = clamp(pixel + vec2<i32>(offset, 0), vec2<i32>(0, 0), vec2<i32>(uniforms.viewport) - vec2<i32>(1, 1));
        let sample_input = ssss_output_to_input_pixel(vec2<u32>(sample_pixel));
        let sample_depth = textureLoad(depthTexture, sample_input, 0);
        let sample_normal = textureLoad(normalTexture, sample_input, 0).xyz;
        let sample_view_position = ssss_reconstruct_view_position(sample_input, sample_depth);
        if (!ssss_geometry_sample_is_compatible(center_depth, center_view_position, center_normal, sample_depth, sample_view_position, sample_normal)) {
            continue;
        }
        let sample_distance = f32(abs(offset));
        let support_weight = clamp(filter_radius + 1.0 - sample_distance, 0.0, 1.0);
        if (support_weight <= 1e-5) {
            continue;
        }
        let weight = ssss_gaussian_weight(offset, max(filter_radius, 0.5)) * support_weight;
        sum = sum + textureLoad(sourceTexture, sample_input, layer, 0) * weight;
        weight_sum = weight_sum + weight;
    }

    textureStore(pingTexture, pixel, layer, select(fallback, sum / max(weight_sum, 1e-5), weight_sum > 0.0));
}
