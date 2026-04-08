struct Camera {
    fov: f32,
    aspectRatio: f32,
    width: u32,
    height: u32,
    position: vec3<f32>,
    direction: vec3<f32>,
    cameraUp: vec3<f32>,
    cameraRight: vec3<f32>,
};

@group(0) @binding(0) var<uniform> camera: Camera;

struct VertexInput {
    @location(0) position: vec3<f32>, // World-space position of the vertex
};

struct VertexOutput {
    @builtin(position) position: vec4<f32>, // Clip-space position
};

/// @brief Constructs a right-handed view matrix from a Camera's position and orientation vectors.
/// @param camera The camera uniform containing position, direction, and up vectors.
/// @returns A 4x4 view matrix that transforms world-space coordinates into camera space.
fn create_view_matrix(camera: Camera) -> mat4x4<f32> {
    let z = normalize(-camera.direction); // Forward
    let x = normalize(cross(camera.cameraUp, z)); // Right
    let y = cross(z, x); // Up

    return mat4x4<f32>(
        vec4<f32>(x, 0.0),
        vec4<f32>(y, 0.0),
        vec4<f32>(z, 0.0),
        vec4<f32>(-dot(x, camera.position), -dot(y, camera.position), -dot(z, camera.position), 1.0)
    );
}

/// @brief Builds a look-at view matrix that orients the camera toward a target point.
/// @param position The world-space camera position.
/// @param t The world-space target point to look at.
/// @param cup The world-space up direction hint used to derive the camera axes.
/// @returns A 4x4 look-at view matrix.
fn look_at(position: vec3<f32>, t: vec3<f32>, cup: vec3<f32>) -> mat4x4<f32> {
    let forward = normalize(t - position);
    let right = normalize(cross(cup, forward));
    let up = cross(forward, right);

    return mat4x4<f32>(
        vec4<f32>(right.x, up.x, -forward.x, 0.0),
        vec4<f32>(right.y, up.y, -forward.y, 0.0),
        vec4<f32>(right.z, up.z, -forward.z, 0.0),
        vec4<f32>(
            -dot(right, position),
            -dot(up, position),
            dot(forward, position),
            1.0
        )
    );
}

/// @brief Constructs a perspective projection matrix with an infinite-far-plane formulation.
/// @param fov The full vertical field-of-view angle in radians.
/// @param aspect The viewport aspect ratio (width / height).
/// @param near The distance to the near clip plane.
/// @param far The distance to the far clip plane.
/// @returns A 4x4 perspective projection matrix.
fn perspective(fov: f32, aspect: f32, near: f32, far: f32) -> mat4x4<f32> {
    let f = 1.0 / tan(fov / 2.0);
    let nf = 1.0 / (near - far);

    return mat4x4<f32>(
        vec4<f32>(f / aspect, 0.0, 0.0, 0.0),
        vec4<f32>(0.0, f, 0.0, 0.0),
        vec4<f32>(0.0, 0.0, (far + near) * nf, -1.0),
        vec4<f32>(0.0, 0.0, 2.0 * far * near * nf, 0.0)
    );
}


/// @brief Vertex entry point that transforms a world-space position to clip space using view and perspective matrices.
@vertex
fn vertex_main(input: VertexInput) -> VertexOutput {
    let view = look_at(camera.position, camera.position + camera.direction, camera.cameraUp);
    let proj = perspective(camera.fov, camera.aspectRatio, 0.1, 4.0);

    // Transform the vertex from world space to clip space
    let world_position = vec4<f32>(input.position, 1.0);
    let view_position = view * world_position;
    let clip_position = proj * view_position;

    var output: VertexOutput;
    output.position = clip_position;//vec4<f32>(input.position.xyz, 1.0);
    return output;
}


/// @brief Fragment entry point that outputs a solid red colour for all rasterized fragments.
@fragment
fn fragment_main(@builtin(position) fragCoord: vec4<f32>) -> @location(0) vec4f {
   let index = u32(fragCoord.y) * camera.width + u32(fragCoord.x);
   return vec4f(1.0,0.0,0.0,1.0);
}
