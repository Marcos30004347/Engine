fn hash_color(value: u32) -> vec3<f32> {
    let x = value * 1664525u + 1013904223u;
    let y = (value ^ 0x9e3779b9u) * 22695477u + 1u;
    let z = (value + 0x7f4a7c15u) * 1103515245u + 12345u;
    return vec3<f32>(
        f32(x & 255u) / 255.0,
        f32(y & 255u) / 255.0,
        f32(z & 255u) / 255.0);
}

struct FSInput {
    @location(0) @interpolate(flat) triangleId : u32,
    @location(1) normal : vec3<f32>,
    @location(2) baseColorOpacity : vec4<f32>,
    @location(3) roughnessMetallic : vec2<f32>,
    @location(4) emissive : vec3<f32>,
    @location(5) @interpolate(flat) surfaceFlags : vec4<u32>,
    @location(6) @interpolate(flat) mode : u32,
}

const MODE_TRIANGLE_ID : u32 = 0u;
const MODE_VERTEX_NORMAL : u32 = 1u;
const MODE_VERTEX_ALBEDO : u32 = 2u;
const MODE_VERTEX_ROUGHNESS : u32 = 3u;
const MODE_VERTEX_METALLIC : u32 = 4u;
const MODE_VERTEX_EMISSIVE : u32 = 5u;
const MODE_VERTEX_OPACITY : u32 = 6u;
const MODE_VERTEX_SURFACE_FLAGS : u32 = 7u;

@fragment
fn fs_main(input : FSInput) -> @location(0) vec4<f32> {
    if (input.mode == MODE_VERTEX_NORMAL) {
        let normal = normalize(input.normal);
        return vec4<f32>(normal * 0.5 + vec3<f32>(0.5), 1.0);
    }
    if (input.mode == MODE_VERTEX_ALBEDO) {
        return vec4<f32>(input.baseColorOpacity.rgb, 1.0);
    }
    if (input.mode == MODE_VERTEX_ROUGHNESS) {
        let roughness = clamp(input.roughnessMetallic.x, 0.0, 1.0);
        return vec4<f32>(vec3<f32>(roughness), 1.0);
    }
    if (input.mode == MODE_VERTEX_METALLIC) {
        let metallic = clamp(input.roughnessMetallic.y, 0.0, 1.0);
        return vec4<f32>(vec3<f32>(metallic), 1.0);
    }
    if (input.mode == MODE_VERTEX_EMISSIVE) {
        return vec4<f32>(input.emissive, 1.0);
    }
    if (input.mode == MODE_VERTEX_OPACITY) {
        let opacity = clamp(input.baseColorOpacity.a, 0.0, 1.0);
        return vec4<f32>(vec3<f32>(opacity), 1.0);
    }
    if (input.mode == MODE_VERTEX_SURFACE_FLAGS) {
        let baked = select(0.0, 1.0, (input.surfaceFlags.x & 1u) != 0u);
        let fallback = select(0.0, 1.0, (input.surfaceFlags.x & 2u) != 0u);
        let material = f32(input.surfaceFlags.y & 255u) / 255.0;
        return vec4<f32>(baked, fallback, material, 1.0);
    }

    let color = vec3<f32>(0.15) + vec3<f32>(0.85) * hash_color(input.triangleId);
    return vec4<f32>(color, 1.0);
}
