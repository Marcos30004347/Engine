struct WorldGIComposeUniforms {
    viewport : vec2<u32>,
    enabled : u32,
    diffuseEnabled : u32,
    glossyEnabled : u32,
    indirectEnabled : u32,
    diffuseWeight : f32,
    glossyWeight : f32,
    indirectIntensity : f32,
    clampValue : f32,
    glossyMaxRoughness : f32,
    _padding0 : f32,
};

@group(0) @binding(0) var<uniform> uniforms : WorldGIComposeUniforms;
@group(0) @binding(1) var diffuseSourceTexture : texture_2d<f32>;
@group(0) @binding(2) var glossySourceTexture : texture_2d<f32>;
@group(0) @binding(3) var materialPropertiesTexture : texture_2d<f32>;
@group(0) @binding(4) var diffuseLightingTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(5) var glossyRadianceTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(6) var indirectLightingTexture : texture_storage_2d<rgba16float, write>;

fn load_clamped(textureValue : texture_2d<f32>, pixel : vec2<i32>) -> vec4<f32> {
    let value = textureLoad(textureValue, pixel, 0);
    let clampValue = max(uniforms.clampValue, 0.0);
    return vec4<f32>(clamp(value.rgb, vec3<f32>(0.0), vec3<f32>(clampValue)), clamp(value.a, 0.0, 1.0));
}

@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) gid : vec3<u32>) {
    if (gid.x >= uniforms.viewport.x || gid.y >= uniforms.viewport.y) {
        return;
    }

    let pixel = vec2<i32>(gid.xy);
    var diffuse = vec4<f32>(0.0);
    var glossy = vec4<f32>(0.0);
    if (uniforms.enabled != 0u) {
        if (uniforms.diffuseEnabled != 0u) {
            diffuse = load_clamped(diffuseSourceTexture, pixel) * vec4<f32>(vec3<f32>(uniforms.diffuseWeight), 1.0);
        }
        if (uniforms.glossyEnabled != 0u) {
            let materialProperties = textureLoad(materialPropertiesTexture, pixel, 0);
            let roughness = clamp(materialProperties.x, 0.0, 1.0);
            let glossyRoughnessWeight = select(1.0, 0.0, roughness > clamp(uniforms.glossyMaxRoughness, 0.0, 1.0));
            glossy = load_clamped(glossySourceTexture, pixel) * vec4<f32>(vec3<f32>(uniforms.glossyWeight * glossyRoughnessWeight), 1.0);
        }
    }

    var indirect = vec4<f32>(0.0);
    if (uniforms.enabled != 0u && uniforms.indirectEnabled != 0u) {
        let confidence = max(diffuse.a, glossy.a);
        indirect = vec4<f32>((diffuse.rgb + glossy.rgb) * uniforms.indirectIntensity, confidence);
    }

    textureStore(diffuseLightingTexture, pixel, diffuse);
    textureStore(glossyRadianceTexture, pixel, glossy);
    textureStore(indirectLightingTexture, pixel, indirect);
}
