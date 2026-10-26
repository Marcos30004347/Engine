#include "atmosphere_common.wgsl"
#include "atmosphere_terrain_shadow.wgsl"

struct AerialPerspectiveUniforms {
    atmosphere : AtmosphereParameters,
    // xyz volume dimensions, w integration samples per local depth segment.
    dispatch : vec4<u32>,
    // xy render viewport, z enabled.
    viewport : vec4<u32>,
    // x camera radius km, y maximum distance km, z distribution scale km,
    // w temporal sequence index.
    cameraDistance : vec4<f32>,
    localUp : vec4<f32>,
    cameraToWorld : mat4x4<f32>,
    focalPrincipal : vec4<f32>,
    terrainShadowControl : vec4<f32>,
    terrainShadowFade : vec4<f32>,
    // x extinction/km at ground, y exponential falloff km, z cutoff km,
    // w enabled.
    volumetricFogControl : vec4<f32>,
    // rgb single-scattering albedo, w anisotropy.
    volumetricFogAlbedoPhase : vec4<f32>,
    // x fully dense distance km, y fade end km. This hands distant haze back
    // to the physical atmosphere instead of building an opaque local-fog wall.
    volumetricFogDistance : vec4<f32>,
}

@group(0) @binding(0) var<uniform> uniforms : AerialPerspectiveUniforms;
@group(0) @binding(1) var transmittanceLut : texture_2d<f32>;
@group(0) @binding(2) var multipleScatteringLut : texture_2d<f32>;
@group(0) @binding(3) var outputLut : texture_storage_3d<rgba16float, write>;
@group(0) @binding(4) var terrainShadowMap : texture_depth_2d;

fn atmosphere_aerial_view_direction(froxel : vec2<u32>) -> vec3<f32> {
    let screen = (vec2<f32>(froxel) + vec2<f32>(0.5)) /
        vec2<f32>(uniforms.dispatch.xy) * vec2<f32>(uniforms.viewport.xy);
    let principal = vec2<f32>(uniforms.viewport.xy) * 0.5;
    let cameraDirection = vec3<f32>(
        (screen.x - principal.x) /
            max(abs(uniforms.focalPrincipal.x), 1.0e-6),
        (screen.y - principal.y) /
            max(abs(uniforms.focalPrincipal.y), 1.0e-6),
        1.0);
    return atmosphere_safe_normalize(
        (uniforms.cameraToWorld * vec4<f32>(cameraDirection, 0.0)).xyz,
        uniforms.localUp.xyz);
}

fn atmosphere_aerial_hash(value : vec3<f32>) -> f32 {
    // R2-like spatiotemporal rotation. Each segment gets an independent
    // stratified offset, while the temporal resolve accumulates the sequence.
    return fract(52.9829189 * fract(dot(value,
        vec3<f32>(0.06711056, 0.00583715, 0.75487766))));
}

// One invocation owns a complete view column. The old implementation launched
// one invocation per endpoint and re-marched [camera, endpoint] for every z
// slice. Integrating ordered local segments makes the volume composable and
// reduces the normal desktop path from depth*samples to depth*localSamples.
@compute @workgroup_size(8, 8, 1)
fn buildAtmosphereAerialPerspective(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    if (any(globalId.xy >= uniforms.dispatch.xy)) { return; }
    if (uniforms.viewport.z == 0u ||
        uniforms.cameraDistance.x < uniforms.atmosphere.radiiMieG.x ||
        uniforms.cameraDistance.x > uniforms.atmosphere.radiiMieG.y) {
        for (var z = 0u; z < uniforms.dispatch.z; z += 1u) {
            textureStore(outputLut,
                vec3<i32>(vec2<i32>(globalId.xy), i32(z)),
                vec4<f32>(0.0, 0.0, 0.0, 1.0));
        }
        return;
    }

    let up = atmosphere_safe_normalize(uniforms.localUp.xyz,
        vec3<f32>(0.0, 1.0, 0.0));
    let direction = atmosphere_aerial_view_direction(globalId.xy);
    let directionTowardSun = atmosphere_safe_normalize(
        -uniforms.atmosphere.sunDirectionIntensity.xyz, up);
    let origin = up * uniforms.cameraDistance.x;
    let topDistance = atmosphere_nearest_positive(atmosphere_ray_sphere(origin,
        direction, uniforms.atmosphere.radiiMieG.y));
    let groundDistance = atmosphere_nearest_positive(atmosphere_ray_sphere(origin,
        direction, uniforms.atmosphere.radiiMieG.x));
    var boundaryDistance = uniforms.cameraDistance.y;
    if (topDistance > 0.0) { boundaryDistance = min(boundaryDistance, topDistance); }
    if (groundDistance > 0.0) {
        boundaryDistance = min(boundaryDistance, groundDistance);
    }

    let phaseCosine = dot(direction, directionTowardSun);
    let rayleighPhase = atmosphere_rayleigh_phase(phaseCosine);
    let miePhase = atmosphere_mie_phase(phaseCosine,
        uniforms.atmosphere.radiiMieG.w);
    let fogPhase = atmosphere_mie_phase(phaseCosine,
        uniforms.volumetricFogAlbedoPhase.w);
    let localSamples = clamp(uniforms.dispatch.w, 1u, 4u);
    var throughput = vec3<f32>(1.0);
    var atmosphereRadiance = vec3<f32>(0.0);
    var fogRadiance = vec3<f32>(0.0);

    for (var z = 0u; z < uniforms.dispatch.z; z += 1u) {
        let segmentStart = min(atmosphere_aerial_distance(
            f32(z) / f32(uniforms.dispatch.z), uniforms.cameraDistance.y,
            uniforms.cameraDistance.z), boundaryDistance);
        let segmentEnd = min(atmosphere_aerial_distance(
            f32(z + 1u) / f32(uniforms.dispatch.z), uniforms.cameraDistance.y,
            uniforms.cameraDistance.z), boundaryDistance);
        let segmentLength = max(segmentEnd - segmentStart, 0.0);
        let subsegmentLength = segmentLength / f32(localSamples);

        for (var sample = 0u; sample < 4u; sample += 1u) {
            if (sample >= localSamples || subsegmentLength <= 0.0) { break; }
            let sequence = vec3<f32>(vec2<f32>(globalId.xy),
                f32(z * 5u + sample) + uniforms.cameraDistance.w * 0.61803399);
            let jitter = atmosphere_aerial_hash(sequence);
            let sampleDistance = segmentStart +
                (f32(sample) + jitter) * subsegmentLength;
            let position = origin + direction * sampleDistance;
            let radius = length(position);
            let sampleUp = position / max(radius, ATMOSPHERE_EPSILON);
            let altitude = radius - uniforms.atmosphere.radiiMieG.x;
            let medium = atmosphere_medium(uniforms.atmosphere, altitude);
            let fogEnabled = uniforms.volumetricFogControl.w > 0.5 &&
                altitude < uniforms.volumetricFogControl.z;
            let fogDistanceWeight = 1.0 - smoothstep(
                uniforms.volumetricFogDistance.x,
                max(uniforms.volumetricFogDistance.y,
                    uniforms.volumetricFogDistance.x + 1.0e-3),
                sampleDistance);
            let fogDensity = select(0.0,
                uniforms.volumetricFogControl.x * exp(-max(altitude, 0.0) /
                    max(uniforms.volumetricFogControl.y, 1.0e-4)) *
                    fogDistanceWeight, fogEnabled);
            let fogExtinction = vec3<f32>(fogDensity);
            let fogScattering = fogExtinction *
                uniforms.volumetricFogAlbedoPhase.rgb;
            let extinction = medium.extinction + fogExtinction;
            let transmittance = exp(-extinction * subsegmentLength);
            let integral = select(vec3<f32>(subsegmentLength),
                (vec3<f32>(1.0) - transmittance) /
                    max(extinction, vec3<f32>(ATMOSPHERE_EPSILON)),
                extinction > vec3<f32>(ATMOSPHERE_EPSILON));
            let sunCosine = dot(sampleUp, directionTowardSun);
            var sunTransmittance = vec3<f32>(0.0);
            if (!atmosphere_planet_occludes(position, directionTowardSun,
                    uniforms.atmosphere.radiiMieG.x)) {
                sunTransmittance = atmosphere_sample_transmittance(
                    transmittanceLut, uniforms.atmosphere, radius, sunCosine);
                sunTransmittance *= atmosphere_terrain_shadow_visibility(
                    terrainShadowMap, position - origin, up, directionTowardSun,
                    uniforms.terrainShadowControl, uniforms.terrainShadowFade);
            }
            let atmosphericDirectSource = sunTransmittance * (
                medium.rayleighScattering * rayleighPhase +
                medium.mieScattering * miePhase);
            let fogDirectSource = sunTransmittance * fogScattering * fogPhase;
            let multipleRadiance = atmosphere_sample_multiple_scattering(
                multipleScatteringLut, uniforms.atmosphere, altitude, sunCosine);
            atmosphereRadiance += throughput *
                (atmosphericDirectSource + multipleRadiance * medium.scattering) *
                integral;
            fogRadiance += throughput *
                (fogDirectSource + multipleRadiance * fogScattering) * integral;
            throughput *= transmittance;
        }

        // atmosphereRadianceScale bridges the physical sky to this sample's
        // display-referred output because it has no final tone mapper. Applying
        // it to authored local fog made that fog eight times brighter than the
        // terrain light and created a white wall in the distance.
        let displayRadiance = (atmosphereRadiance *
            uniforms.atmosphere.groundAlbedoRadianceScale.w + fogRadiance) *
            uniforms.atmosphere.sunColorAngularRadius.xyz *
            uniforms.atmosphere.sunDirectionIntensity.w;
        textureStore(outputLut,
            vec3<i32>(vec2<i32>(globalId.xy), i32(z)), vec4<f32>(
            max(displayRadiance, vec3<f32>(0.0)),
            clamp(dot(throughput, vec3<f32>(1.0 / 3.0)), 0.0, 1.0)));
    }
}
