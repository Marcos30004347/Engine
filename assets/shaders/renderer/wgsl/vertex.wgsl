struct VertexInput {
    @location(0) position: vec3<f32>,
    @location(1) normal: vec3<f32>, 
    @location(2) uv: vec2<f32>, 
};

/// @brief Vertex entry point that passes the XY components of the input position directly to clip space.
@vertex
fn main(input: VertexInput) -> @builtin(position) vec4f {
    return vec4f(input.position.xy, 0.0, 1.0);
}