const MATERIAL_GBUFFER_FLAG_NONE : u32 = 0u;
const MATERIAL_GBUFFER_FLAG_HAS_BASE_COLOR_TEXTURE : u32 = 1u << 0u;
const MATERIAL_GBUFFER_FLAG_HAS_NORMAL_TEXTURE : u32 = 1u << 1u;
const MATERIAL_GBUFFER_FLAG_HAS_EMISSIVE_TEXTURE : u32 = 1u << 2u;
const MATERIAL_GBUFFER_FLAG_ALPHA_MASKED : u32 = 1u << 3u;

struct MaterialGBufferOutput {
    // rgb = base color, a = opacity
    baseColorOpacity : vec4<f32>,

    // xyz = shading normal, w = roughness
    normalRoughness : vec4<f32>,

    // rgb = emitted radiance, w = metallic
    emissiveMetallic : vec4<f32>,

    // x = flags, yzw reserved
    flags : vec4<u32>,
};

fn material_gbuffer_make_default() -> MaterialGBufferOutput {
    var output : MaterialGBufferOutput;
    output.baseColorOpacity = vec4<f32>(0.85, 0.85, 0.85, 1.0);
    output.normalRoughness = vec4<f32>(0.0, 0.0, 1.0, 1.0);
    output.emissiveMetallic = vec4<f32>(0.0, 0.0, 0.0, 0.0);
    output.flags = vec4<u32>(MATERIAL_GBUFFER_FLAG_NONE, 0u, 0u, 0u);
    return output;
}

fn material_gbuffer_make(
    baseColorOpacity : vec4<f32>,
    shadingNormal : vec3<f32>,
    roughness : f32,
    metallic : f32,
    emissiveRadiance : vec3<f32>,
    flags : u32
) -> MaterialGBufferOutput {
    var output : MaterialGBufferOutput;
    output.baseColorOpacity = baseColorOpacity;
    output.normalRoughness = vec4<f32>(shadingNormal, roughness);
    output.emissiveMetallic = vec4<f32>(emissiveRadiance, metallic);
    output.flags = vec4<u32>(flags, 0u, 0u, 0u);
    return output;
}

fn material_gbuffer_normal(output : MaterialGBufferOutput) -> vec3<f32> {
    let normal = output.normalRoughness.xyz;
    let len = length(normal);
    if (len > 1e-6) {
        return normal / len;
    }
    return vec3<f32>(0.0, 0.0, 1.0);
}

fn material_gbuffer_direct_only_color(output : MaterialGBufferOutput, directLighting : vec3<f32>) -> vec4<f32> {
    return vec4<f32>(
        output.baseColorOpacity.rgb * max(directLighting, vec3<f32>(0.0)) + output.emissiveMetallic.rgb,
        output.baseColorOpacity.a
    );
}
