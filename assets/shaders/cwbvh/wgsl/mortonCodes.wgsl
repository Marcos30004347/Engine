// struct Triangle {
//     vertice0: u32,
//     vertice1: u32,
//     vertice2: u32,
//     normal0: u32,
//     normal1: u32,
//     normal2: u32,
//     uv0: u32,
//     uv1: u32,
//     uv2: u32,
// };

struct AABB {
    minPoint: vec3<f32>,
    maxPoint: vec3<f32>,
};

struct Uniforms {
    aabb: AABB,
    num : u32,
};

struct TriangleWoop {
    v0: vec4<f32>,
    v1: vec4<f32>,
    v2: vec4<f32>,
};

@group(0) @binding(0) var<uniform> uniforms : Uniforms;
@group(0) @binding(1) var<storage, read> aabbs : array<AABB>;
@group(0) @binding(2) var<storage, read_write> mortonCodes : array<u32>;
@group(0) @binding(3) var<storage, read_write> indices : array<u32>;

@compute @workgroup_size(64, 1, 1)
fn compute_morton_codes(@builtin(global_invocation_id) global_id : vec3<u32>) {
    var idx = global_id.x;

    if (idx >= uniforms.num) {
        return;
    }

    var cen : vec3<f32> = (aabbs[idx].maxPoint + aabbs[idx].minPoint) * .5f;
    let range = uniforms.aabb.maxPoint - uniforms.aabb.minPoint;

    var normalized = (cen - uniforms.aabb.minPoint) / range;
    
    mortonCodes[idx] = calculate_morton_code(normalized);
    indices[idx] = idx;
}

fn expand_bits(x: u32) -> u32 {
    var v = x & 0x3FFu; // Use only the lower 10 bits for safety
    v = (v | (v << 16u)) & 0x030000FFu;
    v = (v | (v << 8u)) & 0x0300F00Fu;
    v = (v | (v << 4u)) & 0x030C30C3u;
    v = (v | (v << 2u)) & 0x09249249u;
    return v;
}

fn calculate_morton_code(p: vec3<f32>) -> u32 {
    let x = u32(clamp(p.x * 1024.0, 0.0, 1023.0));
    let y = u32(clamp(p.y * 1024.0, 0.0, 1023.0));
    let z = u32(clamp(p.z * 1024.0, 0.0, 1023.0));
    let xx = expand_bits(x);
    let yy = expand_bits(y);
    let zz = expand_bits(z);
    return (xx << 2u) | (yy << 1u) | zz;
}

