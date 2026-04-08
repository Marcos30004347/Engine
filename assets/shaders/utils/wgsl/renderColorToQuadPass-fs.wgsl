struct QuadUniforms {
    ndcMinX : f32,
    ndcMinY : f32,
    ndcMaxX : f32,
    ndcMaxY : f32,
    uvMinX  : f32,  // repurposed: R
    uvMinY  : f32,  // repurposed: G
    uvMaxX  : f32,  // repurposed: B
    uvMaxY  : f32,  // repurposed: A
};

@group(0) @binding(0) var<uniform> u : QuadUniforms;

/// @brief Fragment entry point that outputs a solid RGBA colour sourced from the uniform buffer's UV fields.
@fragment
fn fs_main(@location(0) uv : vec2<f32>) -> @location(0) vec4<f32> {
    return vec4<f32>(u.uvMinX, u.uvMinY, u.uvMaxX, u.uvMaxY);
}