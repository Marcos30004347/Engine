// quad-vs.wgsl
//
// Generates a screen-space quad from 6 vertices (2 triangles) entirely on the
// GPU — no vertex buffer required.  The quad corners are specified in NDC via
// the uniform buffer so the CPU can pass pixel-coordinate rects without
// touching any vertex data.

struct QuadUniforms {
    ndcMinX : f32,
    ndcMinY : f32,
    ndcMaxX : f32,
    ndcMaxY : f32,

    uvMinX  : f32,
    uvMinY  : f32,
    uvMaxX  : f32,
    uvMaxY  : f32,
};

@group(0) @binding(0) var<uniform> u : QuadUniforms;

struct VertexOutput {
    @builtin(position) position : vec4<f32>,
    @location(0)       uv       : vec2<f32>,
};

// Two triangles (CCW winding) covering the quad:
//
//  corner 0 (minX, minY) ------- corner 1 (maxX, minY)
//           |              /              |
//           |           /                |
//  corner 2 (minX, maxY) ------- corner 3 (maxX, maxY)
//
//  vertex index : 0  1  2  3  4  5
//  corner index : 0  2  1  1  2  3

/// @brief Vertex entry point that procedurally generates a screen-space quad from six indices without a vertex buffer.
@vertex
fn vs_main(@builtin(vertex_index) vertexIndex : u32) -> VertexOutput {
    let cornerIndices = array<u32, 6>(0u, 2u, 1u, 1u, 2u, 3u);

    let ndcCorners = array<vec2<f32>, 4>(
        vec2<f32>(u.ndcMinX, u.ndcMinY),  // 0: top-left
        vec2<f32>(u.ndcMaxX, u.ndcMinY),  // 1: top-right
        vec2<f32>(u.ndcMinX, u.ndcMaxY),  // 2: bottom-left
        vec2<f32>(u.ndcMaxX, u.ndcMaxY),  // 3: bottom-right
    );

    let uvCorners = array<vec2<f32>, 4>(
        vec2<f32>(u.uvMinX, u.uvMinY),    // 0: top-left
        vec2<f32>(u.uvMaxX, u.uvMinY),    // 1: top-right
        vec2<f32>(u.uvMinX, u.uvMaxY),    // 2: bottom-left
        vec2<f32>(u.uvMaxX, u.uvMaxY),    // 3: bottom-right
    );

    let ci = cornerIndices[vertexIndex];

    var out : VertexOutput;
    // z = 0.5 — depth test is disabled so this value does not matter.
    out.position = vec4<f32>(ndcCorners[ci], 0.5, 1.0);
    out.uv       = uvCorners[ci];
    return out;
}
