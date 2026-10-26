// Copies the virtual geometry G-buffer out of its attachments into a buffer a
// test can read: per pixel three vec4<u32> words -
//   [depthBits, packedGeometryIdsLo, packedGeometryIdsHi, materialId]
//   [normal.x, normal.y, normal.z, uv.x] as f32 bit patterns
//   [uv.y, 0, 0, 0]
struct Uniforms {
    width  : u32,
    height : u32,
    padding0 : u32,
    padding1 : u32,
}

@group(0) @binding(0) var<uniform> uniforms : Uniforms;
@group(0) @binding(1) var depthTexture : texture_depth_2d;
@group(0) @binding(2) var packedGeometryIdsLo : texture_2d<u32>;
@group(0) @binding(3) var packedGeometryIdsHi : texture_2d<u32>;
@group(0) @binding(4) var materialIdTexture : texture_2d<u32>;
@group(0) @binding(5) var geometryNormalTexture : texture_2d<f32>;
@group(0) @binding(6) var materialUVTexture : texture_2d<f32>;
@group(0) @binding(7) var<storage, read_write> output : array<vec4<u32>>;

@compute @workgroup_size(8, 8, 1)
fn readVirtualGeometryGBuffer(@builtin(global_invocation_id) id : vec3<u32>) {
    if (id.x >= uniforms.width || id.y >= uniforms.height) {
        return;
    }
    let pixel = vec2<i32>(id.xy);
    let index = (id.y * uniforms.width + id.x) * 3u;
    let depth = textureLoad(depthTexture, pixel, 0);
    let normal = textureLoad(geometryNormalTexture, pixel, 0);
    let uv = textureLoad(materialUVTexture, pixel, 0);
    output[index + 0u] = vec4<u32>(
        bitcast<u32>(max(depth, 0.0)),
        textureLoad(packedGeometryIdsLo, pixel, 0).x,
        textureLoad(packedGeometryIdsHi, pixel, 0).x,
        textureLoad(materialIdTexture, pixel, 0).x);
    output[index + 1u] = vec4<u32>(
        bitcast<u32>(normal.x), bitcast<u32>(normal.y), bitcast<u32>(normal.z), bitcast<u32>(uv.x));
    output[index + 2u] = vec4<u32>(bitcast<u32>(uv.y), 0u, 0u, 0u);
}
