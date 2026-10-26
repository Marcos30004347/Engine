struct Uniforms {
    primitiveCount: u32,
    _padding0: u32,
    _padding1: u32,
    _padding2: u32,
};

struct AABB {
    minPoint: vec3<f32>,
    _padding0: f32,
    maxPoint: vec3<f32>,
    _padding1: f32,
};

struct VertexPosition {
    position: vec4<f32>,
};

@group(0) @binding(0) var<uniform> uniforms : Uniforms;
@group(0) @binding(1) var<storage, read> indices : array<u32>;
@group(0) @binding(2) var<storage, read> vertices : array<VertexPosition>;
@group(0) @binding(3) var<storage, read_write> aabbs : array<AABB>;

@compute @workgroup_size(64, 1, 1)
fn build_triangle_aabbs(@builtin(global_invocation_id) global_id : vec3<u32>) {
    let primitiveIndex = global_id.x;
    if primitiveIndex >= uniforms.primitiveCount {
        return;
    }

    let indexBase = primitiveIndex * 3u;
    let i0 = indices[indexBase + 0u];
    let i1 = indices[indexBase + 1u];
    let i2 = indices[indexBase + 2u];

    let p0 = vertices[i0].position.xyz;
    let p1 = vertices[i1].position.xyz;
    let p2 = vertices[i2].position.xyz;

    aabbs[primitiveIndex].minPoint = min(p0, min(p1, p2));
    aabbs[primitiveIndex].maxPoint = max(p0, max(p1, p2));
}
