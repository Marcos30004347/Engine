#include "atmosphere_common.wgsl"

struct TransmittanceUniforms {
    atmosphere : AtmosphereParameters,
    dispatch : vec4<u32>, // xy dimensions, z integration steps, w rebuild.
}

@group(0) @binding(0) var<uniform> uniforms : TransmittanceUniforms;
@group(0) @binding(1) var outputLut : texture_storage_2d<rgba16float, write>;

@compute @workgroup_size(8, 8, 1)
fn buildAtmosphereTransmittance(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let pixel = globalId.xy;
    if (uniforms.dispatch.w == 0u || any(pixel >= uniforms.dispatch.xy)) {
        return;
    }
    let uv = (vec2<f32>(pixel) + vec2<f32>(0.5)) /
        vec2<f32>(uniforms.dispatch.xy);
    let radiusCosine = atmosphere_transmittance_parameters(
        uniforms.atmosphere, uv);
    let radius = radiusCosine.x;
    let cosine = radiusCosine.y;
    let direction = vec3<f32>(atmosphere_safe_sqrt(1.0 - cosine * cosine),
        cosine, 0.0);
    let origin = vec3<f32>(0.0, radius, 0.0);
    let distance = atmosphere_distance_to_top(uniforms.atmosphere, radius, cosine);
    let steps = max(uniforms.dispatch.z, 1u);
    let stepLength = distance / f32(steps);
    var opticalDepth = vec3<f32>(0.0);
    for (var step = 0u; step < 256u; step += 1u) {
        if (step >= steps) { break; }
        let sampleDistance = (f32(step) + 0.5) * stepLength;
        let position = origin + direction * sampleDistance;
        let altitude = length(position) - uniforms.atmosphere.radiiMieG.x;
        opticalDepth += atmosphere_medium(uniforms.atmosphere, altitude).extinction *
            stepLength;
    }
    let transmittance = clamp(exp(-opticalDepth), vec3<f32>(0.0), vec3<f32>(1.0));
    textureStore(outputLut, vec2<i32>(pixel), vec4<f32>(transmittance, 1.0));
}
