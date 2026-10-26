#include "atmosphere_common.wgsl"

struct MultipleScatteringUniforms {
    atmosphere : AtmosphereParameters,
    dispatch : vec4<u32>, // xy dimensions, z directions, w rebuild.
    integration : vec4<u32>, // x steps per direction.
}

@group(0) @binding(0) var<uniform> uniforms : MultipleScatteringUniforms;
@group(0) @binding(1) var transmittanceLut : texture_2d<f32>;
@group(0) @binding(2) var outputLut : texture_storage_2d<rgba16float, write>;

@compute @workgroup_size(8, 8, 1)
fn buildAtmosphereMultipleScattering(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let pixel = globalId.xy;
    if (uniforms.dispatch.w == 0u || any(pixel >= uniforms.dispatch.xy)) {
        return;
    }
    let uv = (vec2<f32>(pixel) + vec2<f32>(0.5)) /
        vec2<f32>(uniforms.dispatch.xy);
    let altitude = uv.y * uniforms.atmosphere.radiiMieG.z;
    let radius = uniforms.atmosphere.radiiMieG.x + altitude;
    let origin = vec3<f32>(0.0, radius, 0.0);
    let sunCosine = uv.x * 2.0 - 1.0;
    let directionTowardSun = vec3<f32>(
        atmosphere_safe_sqrt(1.0 - sunCosine * sunCosine), sunCosine, 0.0);
    let directionCount = max(uniforms.dispatch.z, 1u);
    let stepCount = max(uniforms.integration.x, 1u);
    var secondOrder = vec3<f32>(0.0);
    var transfer = vec3<f32>(0.0);
    for (var directionIndex = 0u; directionIndex < 64u;
         directionIndex += 1u) {
        if (directionIndex >= directionCount) { break; }
        let t = (f32(directionIndex) + 0.5) / f32(directionCount);
        let y = 1.0 - 2.0 * t;
        let radial = atmosphere_safe_sqrt(1.0 - y * y);
        let phi = f32(directionIndex) * 2.399963229728653;
        let direction = vec3<f32>(radial * cos(phi), y, radial * sin(phi));
        let topInterval = atmosphere_ray_sphere(origin, direction,
            uniforms.atmosphere.radiiMieG.y);
        let groundDistance = atmosphere_nearest_positive(atmosphere_ray_sphere(
            origin, direction, uniforms.atmosphere.radiiMieG.x));
        let topDistance = atmosphere_nearest_positive(topInterval);
        let hitsGround = groundDistance > 0.0 &&
            (topDistance < 0.0 || groundDistance < topDistance);
        let pathDistance = select(topDistance, groundDistance, hitsGround);
        let stepLength = max(pathDistance, 0.0) / f32(stepCount);
        var throughput = vec3<f32>(1.0);
        var directionRadiance = vec3<f32>(0.0);
        var directionTransfer = vec3<f32>(0.0);
        for (var step = 0u; step < 64u; step += 1u) {
            if (step >= stepCount) { break; }
            let sampleDistance = (f32(step) + 0.5) * stepLength;
            let position = origin + direction * sampleDistance;
            let sampleRadius = length(position);
            let up = position / max(sampleRadius, ATMOSPHERE_EPSILON);
            let medium = atmosphere_medium(uniforms.atmosphere,
                sampleRadius - uniforms.atmosphere.radiiMieG.x);
            let segmentTransmittance = exp(-medium.extinction * stepLength);
            let segmentIntegral = (vec3<f32>(1.0) - segmentTransmittance) /
                max(medium.extinction, vec3<f32>(ATMOSPHERE_EPSILON));
            let sampleSunCosine = dot(up, directionTowardSun);
            var directTransmittance = vec3<f32>(0.0);
            if (!atmosphere_planet_occludes(position, directionTowardSun,
                    uniforms.atmosphere.radiiMieG.x)) {
                directTransmittance = atmosphere_sample_transmittance(
                    transmittanceLut, uniforms.atmosphere, sampleRadius,
                    sampleSunCosine);
            }
            // Equation 6 in Hillaire 2020: order-two light uses an isotropic
            // phase. The outer uniform-direction average below supplies the
            // second 1/(4*pi) from Equation 5.
            let directSource = directTransmittance * medium.scattering /
                (4.0 * ATMOSPHERE_PI);
            directionRadiance += throughput * directSource * segmentIntegral;
            directionTransfer += throughput * medium.scattering * segmentIntegral;
            throughput *= segmentTransmittance;
        }
        if (hitsGround) {
            let groundPosition = origin + direction * groundDistance;
            let groundNormal = atmosphere_safe_normalize(groundPosition,
                vec3<f32>(0.0, 1.0, 0.0));
            let groundSunCosine = max(dot(groundNormal, directionTowardSun), 0.0);
            let groundSun = atmosphere_sample_transmittance(transmittanceLut,
                uniforms.atmosphere, uniforms.atmosphere.radiiMieG.x,
                dot(groundNormal, directionTowardSun));
            directionRadiance += throughput * uniforms.atmosphere.groundAlbedoRadianceScale.xyz *
                groundSun * groundSunCosine / ATMOSPHERE_PI;
        }
        secondOrder += directionRadiance;
        transfer += directionTransfer;
    }
    secondOrder /= f32(directionCount);
    transfer /= f32(directionCount);
    let denominatorRaw = vec3<f32>(1.0) - transfer;
    let clamped = any(denominatorRaw < vec3<f32>(1.0e-3));
    let multipleScattering = secondOrder / max(denominatorRaw, vec3<f32>(1.0e-3));
    textureStore(outputLut, vec2<i32>(pixel), vec4<f32>(
        max(multipleScattering, vec3<f32>(0.0)), select(0.0, 1.0, clamped)));
}
