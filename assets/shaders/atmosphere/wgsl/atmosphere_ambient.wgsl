#include "atmosphere_common.wgsl"
#include "globalIllumination/worldgi/wgsl/worldgi-types.wgsl"

struct AmbientUniforms {
    localUp : vec4<f32>,
    directionTowardSun : vec4<f32>,
    // x samples, y sky-light index, z light capacity, w enabled.
    dispatch : vec4<u32>,
}

@group(0) @binding(0) var<uniform> uniforms : AmbientUniforms;
@group(0) @binding(1) var skyViewLut : texture_2d<f32>;
@group(0) @binding(2) var<storage, read_write> sceneLights : array<WorldGILight>;

@compute @workgroup_size(1, 1, 1)
fn projectAtmosphereAmbient() {
    if (uniforms.dispatch.w == 0u ||
        uniforms.dispatch.y >= min(uniforms.dispatch.z,
            arrayLength(&sceneLights))) { return; }
    let sampleCount = max(uniforms.dispatch.x, 1u);
    let up = atmosphere_safe_normalize(uniforms.localUp.xyz,
        vec3<f32>(0.0, 1.0, 0.0));
    let towardSun = atmosphere_safe_normalize(
        uniforms.directionTowardSun.xyz, up);
    let weight = 4.0 * ATMOSPHERE_PI / f32(sampleCount);
    var coefficient0 = vec3<f32>(0.0);
    var coefficientX = vec3<f32>(0.0);
    var coefficientY = vec3<f32>(0.0);
    var coefficientZ = vec3<f32>(0.0);
    for (var index = 0u; index < 64u; index += 1u) {
        if (index >= sampleCount) { break; }
        let fraction = (f32(index) + 0.5) / f32(sampleCount);
        let y = 1.0 - 2.0 * fraction;
        let radius = sqrt(max(1.0 - y * y, 0.0));
        let azimuth = f32(index) * 2.39996322972865332;
        let direction = vec3<f32>(
            radius * cos(azimuth), y, radius * sin(azimuth));
        let uv = atmosphere_sky_view_uv(direction, up, towardSun);
        let radiance = max(textureLoad(skyViewLut,
            atmosphere_texture_pixel(uv, textureDimensions(skyViewLut)),
            0).rgb, vec3<f32>(0.0));
        coefficient0 += radiance * 0.2820947918 * weight;
        coefficientX += radiance * (0.4886025119 * direction.x) * weight;
        coefficientY += radiance * (0.4886025119 * direction.y) * weight;
        coefficientZ += radiance * (0.4886025119 * direction.z) * weight;
    }
    // Preserve a small, isotropic playability floor after astronomical
    // twilight. It lives in L0 so every GI consumer agrees and it cannot
    // introduce an arbitrary preferred direction.
    let night = 1.0 - smoothstep(-0.12, 0.0, dot(up, towardSun));
    let nightRadiance = vec3<f32>(0.20, 0.25, 0.40) *
        max(uniforms.localUp.w, 0.0) * night;
    coefficient0 += nightRadiance / 0.2820947918;
    var sky = sceneLights[uniforms.dispatch.y];
    let meanRadiance = max(coefficient0 * 0.2820947918, vec3<f32>(0.0));
    sky.positionRange = vec4<f32>(coefficient0,
        dot(meanRadiance, vec3<f32>(0.2126, 0.7152, 0.0722)));
    sky.directionType = vec4<f32>(coefficientX, 0.0);
    sky.shapeParams = vec4<f32>(coefficientY, 0.0);
    // The LUT contains the physical sun/radiance scaling. Preserve the
    // independent fill-light control in w so reducing ambient does not also
    // weaken the explicitly shadowed directional light.
    sky.colorIntensity = vec4<f32>(coefficientZ,
        max(uniforms.directionTowardSun.w, 0.0));
    sky.flags.x = sky.flags.x | WORLD_GI_LIGHT_FLAG_ENABLED |
        WORLD_GI_LIGHT_FLAG_ATMOSPHERE_SH;
    sceneLights[uniforms.dispatch.y] = sky;
}
