// Copies the virtual geometry G-buffer out of its attachments into a buffer a
// test can read: per pixel four vec4<u32> words -
//   [depthBits, packedGeometryIdsLo, packedGeometryIdsHi, materialId]
//   [normal.x, normal.y, normal.z, uv.x] as f32 bit patterns
//   [uv.y, baseColor.r, baseColor.g, baseColor.b]
//   [motion.x, motion.y, packed city albedo RGBA8, 0]
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
@group(0) @binding(8) var baseColorTexture : texture_2d<f32>;
@group(0) @binding(9) var motionTexture : texture_2d<f32>;
// Optional; a texture of another size is a placeholder and reads as zero.
@group(0) @binding(10) var cityAlbedoTexture : texture_2d<f32>;

@compute @workgroup_size(8, 8, 1)
fn readVirtualGeometryGBuffer(@builtin(global_invocation_id) id : vec3<u32>) {
    if (id.x >= uniforms.width || id.y >= uniforms.height) {
        return;
    }
    let pixel = vec2<i32>(id.xy);
    let index = (id.y * uniforms.width + id.x) * 4u;
    let depth = textureLoad(depthTexture, pixel, 0);
    let normal = textureLoad(geometryNormalTexture, pixel, 0);
    let uv = textureLoad(materialUVTexture, pixel, 0);
    let baseColor = textureLoad(baseColorTexture, pixel, 0);
    let motion = textureLoad(motionTexture, pixel, 0);
    output[index + 0u] = vec4<u32>(
        bitcast<u32>(max(depth, 0.0)),
        textureLoad(packedGeometryIdsLo, pixel, 0).x,
        textureLoad(packedGeometryIdsHi, pixel, 0).x,
        textureLoad(materialIdTexture, pixel, 0).x);
    output[index + 1u] = vec4<u32>(
        bitcast<u32>(normal.x), bitcast<u32>(normal.y), bitcast<u32>(normal.z), bitcast<u32>(uv.x));
    output[index + 2u] = vec4<u32>(bitcast<u32>(uv.y),
        bitcast<u32>(baseColor.r), bitcast<u32>(baseColor.g), bitcast<u32>(baseColor.b));
    var cityAlbedo = 0u;
    if (all(textureDimensions(cityAlbedoTexture) == vec2<u32>(uniforms.width, uniforms.height))) {
        cityAlbedo = pack4x8unorm(textureLoad(cityAlbedoTexture, pixel, 0));
    }
    output[index + 3u] = vec4<u32>(bitcast<u32>(motion.x), bitcast<u32>(motion.y), cityAlbedo, 0u);
}
