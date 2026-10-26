struct HZBReadbackUniforms {
    width : u32,
    height : u32,
    outputOffset : u32,
    reserved : u32,
}

@group(0) @binding(0) var<uniform> uniforms : HZBReadbackUniforms;
@group(0) @binding(1) var sourceMip : texture_2d<f32>;
@group(0) @binding(2) var<storage, read_write> outputDepth : array<f32>;

@compute @workgroup_size(8, 8, 1)
fn readHZBMip(@builtin(global_invocation_id) globalId : vec3<u32>) {
    if (globalId.x >= uniforms.width || globalId.y >= uniforms.height) {
        return;
    }
    let outputIndex = uniforms.outputOffset + globalId.y * uniforms.width + globalId.x;
    outputDepth[outputIndex] = textureLoad(sourceMip, vec2<i32>(globalId.xy), 0).x;
}
