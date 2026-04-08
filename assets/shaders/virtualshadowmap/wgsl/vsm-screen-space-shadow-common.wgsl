struct ScreenSpaceShadowUniforms {
    lightCoordinate: vec4<f32>,
    waveOffset: vec2<i32>,
    depthBounds: vec2<f32>,
    surfaceThickness: f32,
    bilinearThreshold: f32,
    shadowContrast: f32,
    rayDistance: f32,
    farDepthValue: f32,
    nearDepthValue: f32,
    ignoreEdgePixels: u32,
    usePrecisionOffset: u32,
    bilinearSamplingOffsetMode: u32,
    debugOutputEdgeMask: u32,
    debugOutputThreadIndex: u32,
    debugOutputWaveIndex: u32,
    useEarlyOut: u32,
    treatSkippedEdgeSamplesAsLit: u32,
    _padding0: u32,
}

const SSS_WAVE_SIZE: u32 = 64u;
const SSS_SAMPLE_COUNT: u32 = 120u;
const SSS_HARD_SHADOW_SAMPLES: u32 = 4u;
const SSS_FADE_OUT_SAMPLES: u32 = 8u;
const SSS_READ_COUNT: u32 = SSS_SAMPLE_COUNT / SSS_WAVE_SIZE + 2u;
const SSS_SHARED_DEPTH_COUNT: u32 = SSS_READ_COUNT * SSS_WAVE_SIZE;
const SSS_EDGE_SKIP_DEPTH: f32 = 1e20;

@group(0) @binding(0) var<uniform> uniforms: ScreenSpaceShadowUniforms;
@group(0) @binding(1) var depthTexture: texture_depth_2d;
@group(0) @binding(2) var outputTexture: texture_storage_2d<r32float, write>;

var<workgroup> depthData: array<f32, SSS_SHARED_DEPTH_COUNT>;
var<workgroup> ldsEarlyOut: atomic<u32>;

struct WavefrontExtents {
    deltaXY: vec2<f32>,
    pixelXY: vec2<f32>,
    pixelDistance: f32,
    majorAxisX: bool,
}

fn sss_bool(flag: u32) -> bool {
    return flag != 0u;
}

fn sss_saturate(value: f32) -> f32 {
    return clamp(value, 0.0, 1.0);
}

fn sss_saturate4(value: vec4<f32>) -> vec4<f32> {
    return clamp(value, vec4<f32>(0.0), vec4<f32>(1.0));
}

fn sss_sign_i32(value: i32) -> i32 {
    if (value > 0) {
        return 1;
    }
    if (value < 0) {
        return -1;
    }
    return 0;
}

fn sss_output_dimensions_i32() -> vec2<i32> {
    return vec2<i32>(textureDimensions(outputTexture));
}

fn sss_depth_dimensions_i32() -> vec2<i32> {
    return vec2<i32>(textureDimensions(depthTexture));
}

fn sss_is_output_pixel_valid(pixel_xy: vec2<i32>) -> bool {
    let dims = sss_output_dimensions_i32();
    return pixel_xy.x >= 0 && pixel_xy.y >= 0 && pixel_xy.x < dims.x && pixel_xy.y < dims.y;
}

fn sss_load_depth_with_border(pixel_xy: vec2<i32>) -> f32 {
    let dims = sss_depth_dimensions_i32();
    if (pixel_xy.x < 0 || pixel_xy.y < 0 || pixel_xy.x >= dims.x || pixel_xy.y >= dims.y) {
        return uniforms.farDepthValue;
    }
    return textureLoad(depthTexture, pixel_xy, 0);
}

fn sss_depth_is_out_of_bounds(depth: f32) -> bool {
    return depth >= uniforms.depthBounds.y || depth <= uniforms.depthBounds.x;
}

fn compute_wavefront_extents(workgroup_id: vec3<u32>, local_thread_id: u32) -> WavefrontExtents {
    var xy = vec2<i32>(workgroup_id.yz) * i32(SSS_WAVE_SIZE) + uniforms.waveOffset;

    let light_xy = floor(uniforms.lightCoordinate.xy) + vec2<f32>(0.5, 0.5);
    let light_xy_fraction = uniforms.lightCoordinate.xy - light_xy;
    let reverse_direction = uniforms.lightCoordinate.w > 0.0;

    let sign_xy = vec2<i32>(sss_sign_i32(xy.x), sss_sign_i32(xy.y));
    let horizontal = abs(xy.x + sign_xy.y) < abs(xy.y - sign_xy.x);

    var axis = vec2<i32>(0, 0);
    axis.x = select(0, sign_xy.y, horizontal);
    axis.y = select(-sign_xy.x, 0, horizontal);

    xy = axis * i32(workgroup_id.x) + xy;
    let xy_f = vec2<f32>(xy);

    let x_axis_major = abs(xy_f.x) > abs(xy_f.y);
    let major_axis = select(xy_f.y, xy_f.x, x_axis_major);
    let major_axis_start = abs(major_axis);
    let major_axis_end = major_axis_start - f32(SSS_WAVE_SIZE);

    var ma_light_frac = select(light_xy_fraction.y, light_xy_fraction.x, x_axis_major);
    if (major_axis > 0.0) {
        ma_light_frac = -ma_light_frac;
    }

    let start_xy = xy_f + light_xy;
    let interpolation_denominator = major_axis_start + ma_light_frac;
    let interpolation_numerator = major_axis_end + ma_light_frac;
    let interpolation_t = select(0.0, interpolation_numerator / interpolation_denominator, abs(interpolation_denominator) > 1e-6);
    let end_xy = mix(uniforms.lightCoordinate.xy, start_xy, interpolation_t);
    let xy_delta = start_xy - end_xy;

    let reverse_mask = select(SSS_WAVE_SIZE - 1u, 0u, reverse_direction);
    let thread_step = f32(local_thread_id ^ reverse_mask);
    let pixel_xy = mix(start_xy, end_xy, thread_step / f32(SSS_WAVE_SIZE));
    let pixel_distance = major_axis_start - thread_step + ma_light_frac;

    return WavefrontExtents(xy_delta, pixel_xy, pixel_distance, x_axis_major);
}

fn write_screen_space_shadow(workgroup_id: vec3<u32>, local_thread_id: u32) {
    var sampling_depth: array<f32, SSS_READ_COUNT>;
    var shadowing_depth: array<f32, SSS_READ_COUNT>;
    var depth_thickness_scale: array<f32, SSS_READ_COUNT>;
    var sample_distance: array<f32, SSS_READ_COUNT>;

    let direction = -uniforms.lightCoordinate.w;
    let z_sign = select(1.0, -1.0, uniforms.nearDepthValue > uniforms.farDepthValue);
    let ignore_edge_pixels = sss_bool(uniforms.ignoreEdgePixels);
    let use_bilinear_sampling_offset_mode = sss_bool(uniforms.bilinearSamplingOffsetMode);
    let treat_skipped_edge_samples_as_lit = sss_bool(uniforms.treatSkippedEdgeSamplesAsLit);
    let active_sample_count = clamp(u32(round(uniforms.rayDistance)), 1u, SSS_SAMPLE_COUNT);
    let extents = compute_wavefront_extents(workgroup_id, local_thread_id);

    var is_edge = false;
    var skip_pixel = false;
    var pixel_xy = extents.pixelXY;
    let write_xy = vec2<i32>(floor(pixel_xy));
    let can_write = sss_is_output_pixel_valid(write_xy);

    for (var i = 0u; i < SSS_READ_COUNT; i = i + 1u) {
        let read_xy = vec2<i32>(floor(pixel_xy));
        let minor_axis = select(pixel_xy.x, pixel_xy.y, extents.majorAxisX);
        var bilinear = fract(minor_axis) - 0.5;

        let bias = select(-1, 1, bilinear > 0.0);
        let offset_xy = select(vec2<i32>(bias, 0), vec2<i32>(0, bias), extents.majorAxisX);

        let depths = vec2<f32>(
            sss_load_depth_with_border(read_xy),
            sss_load_depth_with_border(read_xy + offset_xy));

        depth_thickness_scale[i] = abs(uniforms.farDepthValue - depths.x);

        let use_point_filter = abs(depths.x - depths.y) > depth_thickness_scale[i] * uniforms.bilinearThreshold;
        if (i == 0u) {
            is_edge = use_point_filter;
        }

        if (use_bilinear_sampling_offset_mode) {
            if (use_point_filter) {
                bilinear = 0.0;
            }

            sampling_depth[i] = mix(depths.x, depths.y, abs(bilinear));
            let skipped_edge_depth = select(sampling_depth[i], SSS_EDGE_SKIP_DEPTH, treat_skipped_edge_samples_as_lit);
            shadowing_depth[i] = select(
                sampling_depth[i],
                skipped_edge_depth,
                ignore_edge_pixels && use_point_filter);
        } else {
            sampling_depth[i] = depths.x;

            let skipped_edge_depth = select(depths.x, SSS_EDGE_SKIP_DEPTH, treat_skipped_edge_samples_as_lit);
            let edge_depth = select(depths.x, skipped_edge_depth, ignore_edge_pixels && use_point_filter);
            let shadow_depth = depths.x + abs(depths.x - depths.y) * z_sign;
            shadowing_depth[i] = select(shadow_depth, edge_depth, use_point_filter);
        }

        sample_distance[i] = extents.pixelDistance + f32(SSS_WAVE_SIZE * i) * direction;
        pixel_xy = pixel_xy + extents.deltaXY * direction;
    }

    if (sss_bool(uniforms.useEarlyOut) &&
        !sss_bool(uniforms.debugOutputWaveIndex) &&
        !sss_bool(uniforms.debugOutputThreadIndex) &&
        !sss_bool(uniforms.debugOutputEdgeMask)) {
        skip_pixel = skip_pixel || sss_depth_is_out_of_bounds(sampling_depth[0]);

        // This mirrors the original wave-wide early-out using workgroup coordination
        // until the local WGSL compiler path supports `enable subgroups`.
        if (local_thread_id == 0u) {
            atomicStore(&ldsEarlyOut, 1u);
        }

        workgroupBarrier();

        if (!skip_pixel) {
            atomicStore(&ldsEarlyOut, 0u);
        }

        workgroupBarrier();

        if (atomicLoad(&ldsEarlyOut) != 0u) {
            if (can_write) {
                textureStore(outputTexture, write_xy, vec4<f32>(1.0, 0.0, 0.0, 1.0));
            }
            return;
        }
    }

    for (var i = 0u; i < SSS_READ_COUNT; i = i + 1u) {
        let distance_denominator = select(
            -1e-6,
            1e-6,
            sample_distance[i] >= 0.0 && abs(sample_distance[i]) <= 1e-6);
        let resolved_denominator = select(distance_denominator, sample_distance[i], abs(sample_distance[i]) > 1e-6);
        var stored_depth = (shadowing_depth[i] - uniforms.lightCoordinate.z) / resolved_denominator;

        if (i != 0u && sample_distance[i] <= 0.0) {
            stored_depth = SSS_EDGE_SKIP_DEPTH;
        }

        depthData[i * SSS_WAVE_SIZE + local_thread_id] = stored_depth;
    }

    workgroupBarrier();

    if (!can_write) {
        return;
    }
    if (skip_pixel) {
        textureStore(outputTexture, write_xy, vec4<f32>(1.0, 0.0, 0.0, 1.0));
        return;
    }

    var start_depth = sampling_depth[0];
    if (sss_bool(uniforms.usePrecisionOffset)) {
        start_depth = mix(start_depth, uniforms.farDepthValue, -1.0 / 65535.0);
    }

    let thickness_scale = max(depth_thickness_scale[0], 1e-6);
    let surface_thickness = max(uniforms.surfaceThickness, 1e-6);
    let start_distance_denominator = select(
        -1e-6,
        1e-6,
        sample_distance[0] >= 0.0 && abs(sample_distance[0]) <= 1e-6);
    let resolved_start_denominator = select(start_distance_denominator, sample_distance[0], abs(sample_distance[0]) > 1e-6);
    let perspective_start_depth = (start_depth - uniforms.lightCoordinate.z) / resolved_start_denominator;
    let depth_scale =
        min(sample_distance[0] + direction, 1.0 / surface_thickness) *
        sample_distance[0] /
        thickness_scale;

    let scaled_start_depth = perspective_start_depth * depth_scale - z_sign;
    let sample_index = local_thread_id + 1u;
    let hard_shadow_sample_count = min(SSS_HARD_SHADOW_SAMPLES, active_sample_count);
    let fade_out_sample_count = min(SSS_FADE_OUT_SAMPLES, active_sample_count - hard_shadow_sample_count);
    let fade_out_start = active_sample_count - fade_out_sample_count;

    var shadow_value = vec4<f32>(1.0);
    var hard_shadow = 1.0;

    for (var i = 0u; i < hard_shadow_sample_count; i = i + 1u) {
        let depth_delta = abs(scaled_start_depth - depthData[sample_index + i] * depth_scale);
        hard_shadow = min(hard_shadow, depth_delta);
    }

    for (var i = hard_shadow_sample_count; i < fade_out_start; i = i + 1u) {
        let depth_delta = abs(scaled_start_depth - depthData[sample_index + i] * depth_scale);
        shadow_value[i & 3u] = min(shadow_value[i & 3u], depth_delta);
    }

    for (var i = fade_out_start; i < active_sample_count; i = i + 1u) {
        let depth_delta = abs(scaled_start_depth - depthData[sample_index + i] * depth_scale);
        let fade_out = f32(i + 1u - fade_out_start) / f32(fade_out_sample_count + 1u) * 0.75;
        shadow_value[i & 3u] = min(shadow_value[i & 3u], depth_delta + fade_out);
    }

    shadow_value = sss_saturate4(shadow_value * uniforms.shadowContrast + vec4<f32>(1.0 - uniforms.shadowContrast));
    hard_shadow = sss_saturate(hard_shadow * uniforms.shadowContrast + (1.0 - uniforms.shadowContrast));

    var result = dot(shadow_value, vec4<f32>(0.25, 0.25, 0.25, 0.25));
    result = min(hard_shadow, result);

    if (sss_bool(uniforms.debugOutputEdgeMask)) {
        result = select(0.0, 1.0, is_edge);
    }
    if (sss_bool(uniforms.debugOutputThreadIndex)) {
        result = f32(local_thread_id) / f32(SSS_WAVE_SIZE);
    }
    if (sss_bool(uniforms.debugOutputWaveIndex)) {
        result = fract(f32(workgroup_id.x) / f32(SSS_WAVE_SIZE));
    }

    textureStore(outputTexture, write_xy, vec4<f32>(result, 0.0, 0.0, 1.0));
}
