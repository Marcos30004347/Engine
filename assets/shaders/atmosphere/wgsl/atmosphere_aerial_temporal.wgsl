#include "atmosphere_common.wgsl"

struct AerialTemporalUniforms {
    // xyz froxel dimensions, w history validity.
    dispatchHistory : vec4<u32>,
    // xy current viewport, zw previous viewport.
    viewport : vec4<u32>,
    // x maximum distance km, y distribution scale km, z history weight,
    // w frame index.
    depthHistory : vec4<f32>,
    currentCameraPosition : vec4<f32>,
    previousCameraPosition : vec4<f32>,
    currentCameraToWorld : mat4x4<f32>,
    previousWorldToCamera : mat4x4<f32>,
    currentFocalPrincipal : vec4<f32>,
    previousFocalPrincipal : vec4<f32>,
}

@group(0) @binding(0) var<uniform> uniforms : AerialTemporalUniforms;
@group(0) @binding(1) var currentVolume : texture_3d<f32>;
@group(0) @binding(2) var historyVolume : texture_3d<f32>;
@group(0) @binding(3) var volumeSampler : sampler;
@group(0) @binding(4) var outputVolume : texture_storage_3d<rgba16float, write>;

fn temporal_view_direction(froxel : vec2<u32>) -> vec3<f32> {
    let screen = (vec2<f32>(froxel) + vec2<f32>(0.5)) /
        vec2<f32>(uniforms.dispatchHistory.xy) *
        vec2<f32>(uniforms.viewport.xy);
    let principal = vec2<f32>(uniforms.viewport.xy) * 0.5;
    let cameraDirection = vec3<f32>(
        (screen.x - principal.x) /
            max(abs(uniforms.currentFocalPrincipal.x), 1.0e-6),
        (screen.y - principal.y) /
            max(abs(uniforms.currentFocalPrincipal.y), 1.0e-6), 1.0);
    return normalize((uniforms.currentCameraToWorld *
        vec4<f32>(cameraDirection, 0.0)).xyz);
}

fn temporal_current_bounds(coordinate : vec3<i32>, current : vec4<f32>) -> mat2x4<f32> {
    let dimensions = vec3<i32>(uniforms.dispatchHistory.xyz);
    var minimumValue = current;
    var maximumValue = current;
    let offsets = array<vec3<i32>, 6>(
        vec3<i32>(-1, 0, 0), vec3<i32>(1, 0, 0),
        vec3<i32>(0, -1, 0), vec3<i32>(0, 1, 0),
        vec3<i32>(0, 0, -1), vec3<i32>(0, 0, 1));
    for (var index = 0u; index < 6u; index += 1u) {
        let neighborCoordinate = clamp(coordinate + offsets[index],
            vec3<i32>(0), dimensions - vec3<i32>(1));
        let neighbor = textureLoad(currentVolume, neighborCoordinate, 0);
        minimumValue = min(minimumValue, neighbor);
        maximumValue = max(maximumValue, neighbor);
    }
    let extent = max(maximumValue - minimumValue, vec4<f32>(1.0e-4));
    return mat2x4<f32>(minimumValue - extent * 0.1,
        maximumValue + extent * 0.1);
}

fn temporal_valid_aerial_sample(value : vec4<f32>) -> bool {
    // A cumulative atmosphere endpoint is always finite, has bounded half-
    // float radiance, and stores transmittance in [0,1]. An all-zero texel is
    // an unwritten history allocation, not a physically meaningful endpoint:
    // even fully opaque fog contains scattering, while clear air has alpha 1.
    return all(value == value) &&
        all(abs(value) < vec4<f32>(65504.0)) &&
        value.a >= 0.0 && value.a <= 1.0 &&
        any(abs(value) > vec4<f32>(1.0e-7));
}

@compute @workgroup_size(4, 4, 4)
fn resolveAtmosphereAerialTemporal(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    if (any(globalId >= uniforms.dispatchHistory.xyz)) { return; }
    let coordinate = vec3<i32>(globalId);
    let current = textureLoad(currentVolume, coordinate, 0);
    if (uniforms.dispatchHistory.w == 0u) {
        textureStore(outputVolume, coordinate, current);
        return;
    }

    let endpointDistanceKm = atmosphere_aerial_distance(
        f32(globalId.z + 1u) / f32(uniforms.dispatchHistory.z),
        uniforms.depthHistory.x, uniforms.depthHistory.y);
    let worldDeltaMetres = temporal_view_direction(globalId.xy) *
        endpointDistanceKm * 1000.0;
    let worldPosition = uniforms.currentCameraPosition.xyz + worldDeltaMetres;
    let previousDelta = worldPosition - uniforms.previousCameraPosition.xyz;
    let previousCamera = (uniforms.previousWorldToCamera *
        vec4<f32>(previousDelta, 0.0)).xyz;
    if (previousCamera.z <= 1.0e-3) {
        textureStore(outputVolume, coordinate, current);
        return;
    }
    let previousPixel = vec2<f32>(
        previousCamera.x / previousCamera.z *
            uniforms.previousFocalPrincipal.x +
            f32(uniforms.viewport.z) * 0.5,
        previousCamera.y / previousCamera.z *
            uniforms.previousFocalPrincipal.y +
            f32(uniforms.viewport.w) * 0.5);
    let previousUv = previousPixel / vec2<f32>(uniforms.viewport.zw);
    let previousDistanceKm = length(previousDelta) * 0.001;
    let previousDepth = atmosphere_aerial_depth(previousDistanceKm,
        uniforms.depthHistory.x, uniforms.depthHistory.y);
    if (any(previousUv <= vec2<f32>(0.0)) ||
        any(previousUv >= vec2<f32>(1.0)) || previousDepth >= 1.0) {
        textureStore(outputVolume, coordinate, current);
        return;
    }

    let history = textureSampleLevel(historyVolume, volumeSampler,
        vec3<f32>(previousUv, previousDepth), 0.0);
    // Reject corruption before clamp/mix. clamp(NaN, ...) is still NaN; once
    // copied into history that poison follows camera reprojection and the
    // composite's safety fallback appears as moving holes in volumetric fog.
    if (!temporal_valid_aerial_sample(history)) {
        textureStore(outputVolume, coordinate, current);
        return;
    }
    let bounds = temporal_current_bounds(coordinate, current);
    let clippedHistory = clamp(history, bounds[0], bounds[1]);
    let transmittanceDisagreement = abs(history.a - current.a);
    let currentUv = (vec2<f32>(globalId.xy) + vec2<f32>(0.5)) /
        vec2<f32>(uniforms.dispatchHistory.xy);
    let screenMotion = length(previousUv - currentUv);
    let confidence = exp(-transmittanceDisagreement * 24.0) *
        exp(-screenMotion * 40.0);
    let historyWeight = clamp(uniforms.depthHistory.z, 0.0, 0.95) * confidence;
    textureStore(outputVolume, coordinate,
        mix(current, clippedHistory, historyWeight));
}
