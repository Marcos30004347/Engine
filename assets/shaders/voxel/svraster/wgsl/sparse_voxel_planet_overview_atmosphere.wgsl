#include "atmosphere/wgsl/atmosphere_common.wgsl"

struct OverviewAtmosphereUniforms {
    atmosphere : AtmosphereParameters,
    // xy viewport; z enabled; w integration samples.
    viewport : vec4<u32>,
    // xy inclusive minimum, zw exclusive maximum pixel bounds.
    bounds : vec4<u32>,
    // x diagnostic, y stars enabled, z draw sun disk.
    controls : vec4<u32>,
    // xyz planet-space camera direction, w distance in ground radii.
    cameraDirectionDistance : vec4<f32>,
    // xyz camera right, w vertical focal scale.
    cameraRightFocal : vec4<f32>,
    // xyz camera up, w star intensity.
    cameraUpStars : vec4<f32>,
    // x solar disk luminance scale.
    display : vec4<f32>,
}

@group(0) @binding(0) var<uniform> uniforms : OverviewAtmosphereUniforms;
@group(0) @binding(1) var overviewColor : texture_2d<f32>;
@group(0) @binding(2) var transmittanceLut : texture_2d<f32>;
@group(0) @binding(3) var multipleScatteringLut : texture_2d<f32>;

struct OverviewAtmosphereVertexOutput {
    @builtin(position) position : vec4<f32>,
}

@vertex
fn drawOverviewAtmosphereVertex(
    @builtin(vertex_index) vertexIndex : u32) -> OverviewAtmosphereVertexOutput {
    let uv = vec2<f32>(f32((vertexIndex << 1u) & 2u),
        f32(vertexIndex & 2u));
    let minimum = vec2<f32>(uniforms.bounds.xy);
    let maximum = vec2<f32>(uniforms.bounds.zw);
    let pixel = mix(minimum, maximum, uv);
    let clip = pixel / max(vec2<f32>(uniforms.viewport.xy), vec2<f32>(1.0)) *
        2.0 - 1.0;
    var output : OverviewAtmosphereVertexOutput;
    output.position = vec4<f32>(clip, 0.0, 1.0);
    if (uniforms.viewport.z == 0u) {
        output.position = vec4<f32>(2.0, 2.0, 0.0, 1.0);
    }
    return output;
}

fn overview_atmosphere_hash(value : vec2<f32>) -> vec3<f32> {
    return fract(sin(vec3<f32>(
        dot(value, vec2<f32>(127.1, 311.7)),
        dot(value, vec2<f32>(269.5, 183.3)),
        dot(value, vec2<f32>(419.2, 371.9)))) * 43758.5453);
}

fn overview_atmosphere_star_layer(direction : vec3<f32>, resolution : f32,
    threshold : f32) -> vec3<f32> {
    let uv = vec2<f32>(
        atan2(direction.z, direction.x) / ATMOSPHERE_TWO_PI + 0.5,
        asin(clamp(direction.y, -1.0, 1.0)) / ATMOSPHERE_PI + 0.5);
    let grid = vec2<f32>(resolution, resolution * 0.5);
    let coordinate = uv * grid;
    let cell = floor(coordinate);
    let random = overview_atmosphere_hash(cell);
    let distanceToStar = length(fract(coordinate) - random.xy);
    let point = 1.0 - smoothstep(0.015, 0.055, distanceToStar);
    let exists = step(threshold, random.z);
    let temperature = overview_atmosphere_hash(cell + vec2<f32>(19.0, 71.0));
    let color = mix(vec3<f32>(0.62, 0.75, 1.0),
        vec3<f32>(1.0, 0.78, 0.55), temperature.x);
    return color * point * exists * mix(0.4, 1.0, temperature.y);
}

fn overview_atmosphere_stars(direction : vec3<f32>) -> vec3<f32> {
    if (uniforms.controls.y == 0u) { return vec3<f32>(0.0); }
    return (overview_atmosphere_star_layer(direction, 512.0, 0.995) +
        overview_atmosphere_star_layer(direction, 1024.0, 0.998) * 0.65) *
        max(uniforms.cameraUpStars.w, 0.0);
}

fn overview_atmosphere_view_direction(pixel : vec2<f32>) -> vec3<f32> {
    let dimensions = vec2<f32>(uniforms.viewport.xy);
    let ndc = (pixel + vec2<f32>(0.5)) / max(dimensions, vec2<f32>(1.0)) *
        2.0 - 1.0;
    let aspect = dimensions.x / max(dimensions.y, 1.0);
    let cameraDirection = atmosphere_safe_normalize(
        uniforms.cameraDirectionDistance.xyz, vec3<f32>(0.0, 0.0, 1.0));
    let cameraRight = atmosphere_safe_normalize(
        uniforms.cameraRightFocal.xyz, vec3<f32>(1.0, 0.0, 0.0));
    let cameraUp = atmosphere_safe_normalize(
        uniforms.cameraUpStars.xyz, vec3<f32>(0.0, 1.0, 0.0));
    let focal = max(uniforms.cameraRightFocal.w, 0.001);
    return normalize(-cameraDirection + cameraRight * (ndc.x * aspect / focal) -
        cameraUp * (ndc.y / focal));
}

fn overview_atmosphere_closest_warp(
    fraction : f32, closestFraction : f32) -> f32 {
    // From orbit the density peak is at the ray's closest approach to the
    // planet, not generally at either shell intersection. Uniform midpoint
    // samples can straddle that peak and miss the visible limb completely.
    // Warp both sides quadratically toward it while retaining ordered segment
    // boundaries and exact integration over the complete shell interval.
    let closest = clamp(closestFraction, 0.0, 1.0);
    if (fraction <= closest && closest > ATMOSPHERE_EPSILON) {
        let local = fraction / closest;
        return closest * (1.0 - (1.0 - local) * (1.0 - local));
    }
    let remaining = 1.0 - closest;
    if (remaining > ATMOSPHERE_EPSILON) {
        let local = (fraction - closest) / remaining;
        return closest + remaining * local * local;
    }
    return fraction;
}

@fragment
fn drawOverviewAtmosphereFragment(
    @builtin(position) fragmentPosition : vec4<f32>) -> @location(0) vec4<f32> {
    let pixel = vec2<i32>(fragmentPosition.xy);
    let direction = overview_atmosphere_view_direction(fragmentPosition.xy);
    let groundRadius = uniforms.atmosphere.radiiMieG.x;
    let topRadius = uniforms.atmosphere.radiiMieG.y;
    let cameraDirection = atmosphere_safe_normalize(
        uniforms.cameraDirectionDistance.xyz, vec3<f32>(0.0, 0.0, 1.0));
    let origin = cameraDirection *
        (uniforms.cameraDirectionDistance.w * groundRadius);
    let topInterval = atmosphere_ray_sphere(origin, direction, topRadius);
    let entryDistance = max(topInterval.x, 0.0);
    let exitDistance = topInterval.y;
    let stars = overview_atmosphere_stars(direction);
    if (uniforms.viewport.z == 0u || exitDistance <= entryDistance) {
        return vec4<f32>(stars, 1.0);
    }

    let groundInterval = atmosphere_ray_sphere(origin, direction, groundRadius);
    let source = textureLoad(overviewColor, pixel, 0);
    let hitsGround = groundInterval.x > entryDistance &&
        groundInterval.x < exitDistance;
    let pathEnd = select(exitDistance, groundInterval.x, hitsGround);
    let directionTowardSun = atmosphere_safe_normalize(
        -uniforms.atmosphere.sunDirectionIntensity.xyz,
        vec3<f32>(0.0, 1.0, 0.0));

    var background = select(stars, source.rgb, hitsGround);
    if (!hitsGround && uniforms.controls.z != 0u &&
        uniforms.atmosphere.sunDirectionIntensity.w > 0.0 &&
        dot(direction, directionTowardSun) >=
            cos(uniforms.atmosphere.sunColorAngularRadius.w)) {
        background += uniforms.atmosphere.sunColorAngularRadius.xyz *
            uniforms.atmosphere.sunDirectionIntensity.w *
            uniforms.atmosphere.groundAlbedoRadianceScale.w *
            max(uniforms.display.x, 0.0);
    }

    let steps = clamp(uniforms.viewport.w, 1u, 32u);
    var throughput = vec3<f32>(1.0);
    var radiance = vec3<f32>(0.0);
    let pathLength = max(pathEnd - entryDistance, 0.0);
    let closestDistance = clamp(-dot(origin, direction),
        entryDistance, pathEnd);
    var closestFraction = 0.0;
    if (pathLength > ATMOSPHERE_EPSILON) {
        closestFraction =
            (closestDistance - entryDistance) / pathLength;
    }
    for (var step = 0u; step < 32u; step += 1u) {
        if (step >= steps) { break; }
        let startFraction = overview_atmosphere_closest_warp(
            f32(step) / f32(steps), closestFraction);
        let endFraction = overview_atmosphere_closest_warp(
            f32(step + 1u) / f32(steps), closestFraction);
        let segmentStart = entryDistance + pathLength * startFraction;
        let segmentEnd = entryDistance + pathLength * endFraction;
        let stepLength = segmentEnd - segmentStart;
        let position = origin + direction *
            (segmentStart + stepLength * 0.5);
        let radius = length(position);
        let up = position / max(radius, ATMOSPHERE_EPSILON);
        let altitude = radius - groundRadius;
        let medium = atmosphere_medium(uniforms.atmosphere, altitude);
        let segmentTransmittance = exp(-medium.extinction * stepLength);
        let segmentIntegral = (vec3<f32>(1.0) - segmentTransmittance) /
            max(medium.extinction, vec3<f32>(ATMOSPHERE_EPSILON));
        let sunCosine = dot(up, directionTowardSun);
        var directTransmittance = vec3<f32>(0.0);
        if (!atmosphere_planet_occludes(position, directionTowardSun,
                groundRadius)) {
            directTransmittance = atmosphere_sample_transmittance(
                transmittanceLut, uniforms.atmosphere, radius, sunCosine);
        }
        let phaseCosine = dot(direction, directionTowardSun);
        let directSource = directTransmittance * (
            medium.rayleighScattering * atmosphere_rayleigh_phase(phaseCosine) +
            medium.mieScattering * atmosphere_mie_phase(phaseCosine,
                uniforms.atmosphere.radiiMieG.w));
        let multipleSource = atmosphere_sample_multiple_scattering(
            multipleScatteringLut, uniforms.atmosphere, altitude,
            sunCosine) * medium.scattering;
        radiance += throughput * (directSource + multipleSource) *
            segmentIntegral;
        throughput *= segmentTransmittance;
    }
    radiance *= uniforms.atmosphere.sunColorAngularRadius.xyz *
        uniforms.atmosphere.sunDirectionIntensity.w *
        uniforms.atmosphere.groundAlbedoRadianceScale.w;

    if (uniforms.controls.x == 1u) {
        return vec4<f32>(throughput, 1.0);
    }
    if (uniforms.controls.x == 2u) {
        return vec4<f32>(max(radiance, vec3<f32>(0.0)), 1.0);
    }
    if (uniforms.controls.x == 3u) {
        let shell = select(vec3<f32>(0.08, 0.15, 0.35),
            vec3<f32>(0.1, 0.8, 0.25), hitsGround);
        let boundsMinimum = vec2<f32>(uniforms.bounds.xy);
        let boundsMaximum = vec2<f32>(uniforms.bounds.zw);
        let edgeDistance = min(min(fragmentPosition.x - boundsMinimum.x,
            fragmentPosition.y - boundsMinimum.y), min(
            boundsMaximum.x - fragmentPosition.x,
            boundsMaximum.y - fragmentPosition.y));
        return vec4<f32>(select(shell, vec3<f32>(1.0, 0.7, 0.05),
            edgeDistance < 2.0), 1.0);
    }
    return vec4<f32>(max(background * throughput + radiance,
        vec3<f32>(0.0)), 1.0);
}
