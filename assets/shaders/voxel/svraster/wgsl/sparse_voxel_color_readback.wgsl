struct SparseVoxelColorReadbackUniforms {
    width : u32,
    height : u32,
    reserved0 : u32,
    reserved1 : u32,
}

@group(0) @binding(0) var<uniform> uniforms :
    SparseVoxelColorReadbackUniforms;
@group(0) @binding(1) var inputTexture : texture_2d<f32>;
@group(0) @binding(2) var<storage, read_write> outputPixels :
    array<vec4<f32>>;

@compute @workgroup_size(8, 8, 1)
fn readSparseVoxelColor(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let pixel = globalId.xy;
    if (pixel.x >= uniforms.width || pixel.y >= uniforms.height) {
        return;
    }
    outputPixels[pixel.y * uniforms.width + pixel.x] =
        textureLoad(inputTexture, vec2<i32>(pixel), 0);
}
