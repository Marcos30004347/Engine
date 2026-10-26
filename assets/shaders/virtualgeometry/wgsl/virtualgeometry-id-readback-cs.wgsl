// Copy the virtual geometry visibility ids out of the G-buffer into a buffer.
//
// There is no generic texture-to-buffer copy in the render graph, and the
// packed id targets are colour attachments rather than storage textures, so a
// test that wants to look at the rendered picture needs this one dispatch. It
// carries no rendering logic: one texel in, one word out.

struct Uniforms {
    width : u32,
    height : u32,
    _padding0 : u32,
    _padding1 : u32,
};

@group(0) @binding(0) var<uniform> uniforms : Uniforms;
@group(0) @binding(1) var packedGeometryIdsLo : texture_2d<u32>;
@group(0) @binding(2) var packedGeometryIdsHi : texture_2d<u32>;
@group(0) @binding(3) var<storage, read_write> outputIds : array<vec2<u32>>;

@compute @workgroup_size(8, 8, 1)
fn readVirtualGeometryIds(@builtin(global_invocation_id) id : vec3<u32>) {
    if (id.x >= uniforms.width || id.y >= uniforms.height) {
        return;
    }
    let coordinate = vec2<i32>(i32(id.x), i32(id.y));
    let lo = textureLoad(packedGeometryIdsLo, coordinate, 0).r;
    let hi = textureLoad(packedGeometryIdsHi, coordinate, 0).r;
    outputIds[id.y * uniforms.width + id.x] = vec2<u32>(lo, hi);
}
