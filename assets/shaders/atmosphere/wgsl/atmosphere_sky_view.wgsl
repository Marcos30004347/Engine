#include "atmosphere_common.wgsl"
#include "atmosphere_terrain_shadow.wgsl"

struct SkyViewUniforms {
    atmosphere : AtmosphereParameters,
    dispatch : vec4<u32>, // xy dimensions, z integration steps, w enabled.
    cameraSun : vec4<f32>, // x camera radius km, y sun zenith cosine.
    localUp : vec4<f32>,
    terrainShadowControl : vec4<f32>,
    terrainShadowFade : vec4<f32>,
}

@group(0) @binding(0) var<uniform> uniforms : SkyViewUniforms;
@group(0) @binding(1) var transmittanceLut : texture_2d<f32>;
@group(0) @binding(2) var multipleScatteringLut : texture_2d<f32>;
@group(0) @binding(3) var outputLut : texture_storage_2d<rgba16float, write>;
@group(0) @binding(4) var terrainShadowMap : texture_depth_2d;

@compute @workgroup_size(8, 8, 1)
fn buildAtmosphereSkyView(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let pixel = globalId.xy;
    if (any(pixel >= uniforms.dispatch.xy)) { return; }
    if (uniforms.dispatch.w == 0u) {
        textureStore(outputLut, vec2<i32>(pixel), vec4<f32>(0.0, 0.0, 0.0, 1.0));
        return;
    }
    let uv = (vec2<f32>(pixel) + vec2<f32>(0.5)) /
        vec2<f32>(uniforms.dispatch.xy);
    let sunCosine = clamp(uniforms.cameraSun.y, -1.0, 1.0);
    let directionTowardSun = vec3<f32>(
        atmosphere_safe_sqrt(1.0 - sunCosine * sunCosine), sunCosine, 0.0);
    let direction = atmosphere_sky_view_direction(uniforms.atmosphere, uv,
        sunCosine);
    let origin = vec3<f32>(0.0, uniforms.cameraSun.x, 0.0);
    let topDistance = atmosphere_nearest_positive(atmosphere_ray_sphere(origin,
        direction, uniforms.atmosphere.radiiMieG.y));
    let groundDistance = atmosphere_nearest_positive(atmosphere_ray_sphere(origin,
        direction, uniforms.atmosphere.radiiMieG.x));
    let hitsGround = groundDistance > 0.0 &&
        (topDistance < 0.0 || groundDistance < topDistance);
    let pathDistance = max(select(topDistance, groundDistance, hitsGround), 0.0);
    let steps = max(uniforms.dispatch.z, 1u);
    var throughput = vec3<f32>(1.0);
    var radiance = vec3<f32>(0.0);
    var previousDistance = 0.0;
    for (var step = 0u; step < 64u; step += 1u) {
        if (step >= steps) { break; }
        let normalizedEnd = f32(step + 1u) / f32(steps);
        let segmentEnd = pathDistance * normalizedEnd * normalizedEnd;
        let stepLength = segmentEnd - previousDistance;
        let sampleDistance = previousDistance + stepLength * 0.5;
        previousDistance = segmentEnd;
        let position = origin + direction * sampleDistance;
        let radius = length(position);
        let up = position / max(radius, ATMOSPHERE_EPSILON);
        let altitude = radius - uniforms.atmosphere.radiiMieG.x;
        let medium = atmosphere_medium(uniforms.atmosphere, altitude);
        let segmentTransmittance = exp(-medium.extinction * stepLength);
        let segmentIntegral = (vec3<f32>(1.0) - segmentTransmittance) /
            max(medium.extinction, vec3<f32>(ATMOSPHERE_EPSILON));
        let sampleSunCosine = dot(up, directionTowardSun);
        var directTransmittance = vec3<f32>(0.0);
        if (!atmosphere_planet_occludes(position, directionTowardSun,
                uniforms.atmosphere.radiiMieG.x)) {
            directTransmittance = atmosphere_sample_transmittance(
                transmittanceLut, uniforms.atmosphere, radius, sampleSunCosine);
            let worldUp = atmosphere_safe_normalize(uniforms.localUp.xyz,
                vec3<f32>(0.0, 1.0, 0.0));
            let worldSun = atmosphere_safe_normalize(
                -uniforms.atmosphere.sunDirectionIntensity.xyz, worldUp);
            var sunTangent = worldSun - worldUp * dot(worldSun, worldUp);
            let fallback = atmosphere_safe_normalize(cross(select(
                vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 0.0, 1.0),
                abs(worldUp.z) < 0.9), worldUp), vec3<f32>(1.0, 0.0, 0.0));
            sunTangent = atmosphere_safe_normalize(sunTangent, fallback);
            let bitangent = cross(sunTangent, worldUp);
            let relativeCanonical = position - origin;
            let relativeWorld = sunTangent * relativeCanonical.x +
                worldUp * relativeCanonical.y + bitangent * relativeCanonical.z;
            directTransmittance *= atmosphere_terrain_shadow_visibility(
                terrainShadowMap, relativeWorld, worldUp, worldSun,
                uniforms.terrainShadowControl, uniforms.terrainShadowFade);
        }
        let phaseCosine = dot(direction, directionTowardSun);
        let directSource = directTransmittance * (
            medium.rayleighScattering * atmosphere_rayleigh_phase(phaseCosine) +
            medium.mieScattering * atmosphere_mie_phase(phaseCosine,
                uniforms.atmosphere.radiiMieG.w));
        let multipleSource = atmosphere_sample_multiple_scattering(
            multipleScatteringLut, uniforms.atmosphere, altitude,
            sampleSunCosine) * medium.scattering;
        radiance += throughput * (directSource + multipleSource) * segmentIntegral;
        throughput *= segmentTransmittance;
    }
    radiance *= uniforms.atmosphere.sunColorAngularRadius.xyz *
        uniforms.atmosphere.sunDirectionIntensity.w *
        uniforms.atmosphere.groundAlbedoRadianceScale.w;
    textureStore(outputLut, vec2<i32>(pixel), vec4<f32>(
        max(radiance, vec3<f32>(0.0)), 1.0));
}
