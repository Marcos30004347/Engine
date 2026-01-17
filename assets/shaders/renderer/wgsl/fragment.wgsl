struct Uniforms {
    width: u32,
    height: u32,
};

@group(0) @binding(0) var<uniform> uniforms: Uniforms;
@group(0) @binding(1) var<storage, read_write> colorBuffer: array<vec4<f32>>;

@fragment
fn triangles_rendering(@builtin(position) fragCoord: vec4<f32>) -> @location(0) vec4f {
   let index = u32(fragCoord.y) * uniforms.width + u32(fragCoord.x);
   return vec4f(colorBuffer[index].xyz, 1.0);
}

