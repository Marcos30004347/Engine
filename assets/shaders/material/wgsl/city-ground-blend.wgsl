// CityModelPlan::groundWeights: grass, earth, cobble, normalized per corner.
// Bind the palette as an sRGB texture array (layers follow groundMaterialIds)
// and a repeat/nearest sampler. Texture sampling decodes to linear RGB.
// Outputs linear albedo; the destination sRGB attachment performs encoding.
// This standalone material module is not yet bound to virtual geometry.
struct GroundPalette {
    grassRepeat : vec2<f32>,
    dirtRepeat : vec2<f32>,
    stoneRepeat : vec2<f32>,
    _padding : vec2<f32>,
};
@group(0) @binding(0) var groundSampler : sampler;
@group(0) @binding(1) var groundTextures : texture_2d_array<f32>;
@group(0) @binding(2) var<uniform> palette : GroundPalette;

struct GroundInput {
    @location(0) worldXZ : vec2<f32>,
    @location(1) weights : vec3<f32>,
};

@fragment
fn main(input : GroundInput) -> @location(0) vec4<f32> {
    let nonnegative = max(input.weights, vec3<f32>(0.0));
    let total = dot(nonnegative, vec3<f32>(1.0));
    let weights = select(vec3<f32>(1.0, 0.0, 0.0), nonnegative / max(total, 0.000001), total > 0.000001);
    let grass = textureSample(groundTextures, groundSampler, input.worldXZ / max(palette.grassRepeat, vec2<f32>(0.001)), 0).rgb;
    let dirt = textureSample(groundTextures, groundSampler, input.worldXZ / max(palette.dirtRepeat, vec2<f32>(0.001)), 1).rgb;
    let stone = textureSample(groundTextures, groundSampler, input.worldXZ / max(palette.stoneRepeat, vec2<f32>(0.001)), 2).rgb;
    return vec4<f32>(grass * weights.x + dirt * weights.y + stone * weights.z, 1.0);
}
