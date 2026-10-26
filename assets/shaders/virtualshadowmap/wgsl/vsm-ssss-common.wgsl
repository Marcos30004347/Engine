#ifndef VSM_SSSS_KERNEL_SIZE
#define VSM_SSSS_KERNEL_SIZE 11
#endif

struct ScreenSpaceSoftShadowUniforms {
    viewport: vec2<u32>,
    inputViewport: vec2<u32>,
    layerCount: u32,
    activeLights: u32,
    resolutionScale: u32,
    kernelSize: u32,
    blockerSearchTaps: u32,
    useMinFilter: u32,
    translucentShadowsEnabled: u32,
    defaultLightRadiusWorld: f32,
    penumbraScale: f32,
    grazingThreshold: f32,
    depthRejectThreshold: f32,
    debugMode: u32,
    debugLayer: u32,
    debugValueScale: f32,
    _padding0: u32,
    _padding1: u32,
    pageTableResolution: u32,
    activeLayers: u32,
    cascadeCount: u32,
    physicalPageSize: u32,
    reverseZ: u32,
    fallbackCascadeOffset: u32,
    firstCascadeWorldExtent: f32,
    localLightMipTransitionScale: f32,
    localLightMipBias: f32,
    localLightPageWorldScale: f32,
    directionalShadowBias: f32,
    spotShadowBias: f32,
    pointShadowBias: f32,
    _vsmPadding0: u32,
    _vsmPadding1: u32,
    _vsmPadding2: u32,
}

struct SoftShadowLight {
    layer: u32,
    flags: u32,
    lightRadiusWorld: f32,
    penumbraScale: f32,
}

fn ssss_in_bounds(pixel: vec2<u32>, dims: vec2<u32>) -> bool {
    return pixel.x < dims.x && pixel.y < dims.y;
}

fn ssss_output_to_input_pixel(pixel: vec2<u32>) -> vec2<i32> {
    let uv = (vec2<f32>(pixel) + vec2<f32>(0.5, 0.5)) / max(vec2<f32>(uniforms.viewport), vec2<f32>(1.0, 1.0));
    let input_pixel = clamp(
        uv * vec2<f32>(uniforms.inputViewport),
        vec2<f32>(0.0, 0.0),
        max(vec2<f32>(uniforms.inputViewport) - vec2<f32>(1.0, 1.0), vec2<f32>(0.0, 0.0)));
    return vec2<i32>(input_pixel);
}

fn ssss_saturate(value: f32) -> f32 {
    return clamp(value, 0.0, 1.0);
}

fn ssss_gaussian_weight(offset: i32, radius: f32) -> f32 {
    let sigma = max(radius * 0.5, 0.5);
    let x = f32(offset);
    return exp(-(x * x) / max(2.0 * sigma * sigma, 1e-4));
}

fn ssss_normal_is_valid(encoded_normal: vec3<f32>) -> bool {
    return dot(encoded_normal, encoded_normal) > 1e-8;
}

fn ssss_decode_normal(encoded_normal: vec3<f32>) -> vec3<f32> {
    return encoded_normal * inverseSqrt(max(dot(encoded_normal, encoded_normal), 1e-8));
}

fn ssss_depths_are_compatible(center_depth: f32, sample_depth: f32) -> bool {
    if (center_depth <= 0.0 || sample_depth <= 0.0) {
        return false;
    }

    let depth_delta = abs(sample_depth - center_depth);
    let relative_limit = max(max(abs(center_depth), abs(sample_depth)) * uniforms.depthRejectThreshold, uniforms.depthRejectThreshold * 0.25);
    let reject_limit = min(uniforms.depthRejectThreshold, relative_limit);
    return depth_delta <= reject_limit;
}

fn ssss_normals_are_compatible(center_encoded_normal: vec3<f32>, sample_encoded_normal: vec3<f32>) -> bool {
    let center_valid = ssss_normal_is_valid(center_encoded_normal);
    let sample_valid = ssss_normal_is_valid(sample_encoded_normal);
    if (!center_valid || !sample_valid) {
        return true;
    }

    let center_normal = ssss_decode_normal(center_encoded_normal);
    let sample_normal = ssss_decode_normal(sample_encoded_normal);
    return dot(center_normal, sample_normal) >= 0.35;
}
