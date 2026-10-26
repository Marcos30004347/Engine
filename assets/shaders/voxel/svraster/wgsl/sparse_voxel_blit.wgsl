// Final scale from whatever the graph last produced to the swapchain.
//
// With the CRT chain off, or with its output extent equal to the window, the
// transform below is the identity and this is the plain copy it has always
// been. When the CRT chain simulates the tube at an extent whose aspect differs
// from the window - a 4:3 tube in a 16:9 window - the image is fitted and the
// remainder shows the bezel colour rather than being stretched.

struct BlitUniforms {
    // uv scale xy, uv offset xy. Identity is (1, 1, 0, 0).
    uvScaleOffset : vec4<f32>,
    // colour shown outside the fitted image
    bezel : vec4<f32>,
}

@group(0) @binding(0) var nearestSampler : sampler;
@group(0) @binding(1) var inputTexture : texture_2d<f32>;
@group(0) @binding(2) var<uniform> blit : BlitUniforms;

struct VertexOutput {
    @builtin(position) position : vec4<f32>,
    @location(0) uv : vec2<f32>,
};

@vertex
fn blitVertex(@builtin(vertex_index) vertexIndex : u32) -> VertexOutput {
    let cornerIndices = array<u32, 6>(0u, 2u, 1u, 1u, 2u, 3u);
    let positions = array<vec2<f32>, 4>(
        vec2<f32>(-1.0, -1.0),
        vec2<f32>( 1.0, -1.0),
        vec2<f32>(-1.0,  1.0),
        vec2<f32>( 1.0,  1.0));
    let uvs = array<vec2<f32>, 4>(
        vec2<f32>(0.0, 0.0),
        vec2<f32>(1.0, 0.0),
        vec2<f32>(0.0, 1.0),
        vec2<f32>(1.0, 1.0));
    let corner = cornerIndices[vertexIndex];
    var output : VertexOutput;
    output.position = vec4<f32>(positions[corner], 0.0, 1.0);
    output.uv = uvs[corner];
    return output;
}

@fragment
fn blitFragment(@location(0) uv : vec2<f32>) -> @location(0) vec4<f32> {
    // Identity when scale is (1, 1) and offset is (0, 0): (uv - 0) / 1 is
    // bit-preserving, and the bounds test cannot fire, so the no-letterbox path
    // is exactly the sample it used to be.
    let source = (uv - blit.uvScaleOffset.zw) / blit.uvScaleOffset.xy;
    if (source.x < 0.0 || source.x > 1.0 || source.y < 0.0 || source.y > 1.0) {
        return blit.bezel;
    }
    return textureSample(inputTexture, nearestSampler, source);
}
