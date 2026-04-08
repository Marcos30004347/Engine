enable subgroups;

struct AABB {
    minPoint: vec3<f32>,
    maxPoint: vec3<f32>,
};

struct InstancesData {
    modelMatrix: mat4x4<f32>,
    hierarchyRoot: u32,
    aabb: AABB,
};

struct LODBounds {
   center: vec4<f32>,
   radius: f32,
   error: f32,
};

struct ClusterData {
  selfCenterX : f32,
  selfCenterY : f32,
  selfCenterZ : f32,
  selfRadius : f32,
  selfError : f32,

  parentCenterX : f32,
  parentCenterY : f32,
  parentCenterZ : f32,
  parentError : f32,
  parentRadius : f32,

  indicesCount : u32,
  indicesOffset : u32,
};

struct Uniforms {
    view: mat4x4<f32>,
    proj: mat4x4<f32>,
    viewPosition: vec4<f32>,
    viewport: vec2<u32>,
}

@group(0) @binding(0) var<uniform> uniforms: Uniforms;
@group(0) @binding(1) var<storage, read> instancesData: array<InstancesData>;
@group(0) @binding(2) var<storage, read> clusterData: array<ClusterData>;

struct VertexInput {
    @location(0) position: vec3<f32>,
    @location(1) normal: vec3<f32>, 
    @location(2) uv: vec2<f32>, 
};

struct VertexOutput {
    @builtin(position) position: vec4<f32>, 
    @location(0) normal: vec4<f32>,
    @location(1) uv: vec4<f32>,
    @location(2) color: vec4<f32>,
};

fn rand(seed: vec2<f32>) -> f32 {
    let a: f32 = 12.9898;
    let b: f32 = 78.233;
    let c: f32 = 43758.5453;
    let dt: f32 = dot(seed, vec2<f32>(a, b));
    let sn: f32 = fract(sin(dt) * c);
    return sn;
}

fn randomColor(seed: f32) -> vec4<f32> {
    let r = rand(vec2<f32>(seed, 0.1));
    let g = rand(vec2<f32>(seed, 0.2));
    let b = rand(vec2<f32>(seed, 0.3));
    let a = 1.0;
    return vec4<f32>(r, g, b, a);
}

// fn create_view_matrix(camera: Camera) -> mat4x4<f32> {
//     let z = normalize(-camera.direction); // Forward
//     let x = normalize(cross(camera.cameraUp, z)); // Right
//     let y = cross(z, x); // Up

//     return mat4x4<f32>(
//         vec4<f32>(x, 0.0),
//         vec4<f32>(y, 0.0),
//         vec4<f32>(z, 0.0),
//         vec4<f32>(-dot(x, camera.position), -dot(y, camera.position), -dot(z, camera.position), 1.0)
//     );
// }

// fn look_at(position: vec3<f32>, t: vec3<f32>, cup: vec3<f32>) -> mat4x4<f32> {
//     let forward = normalize(t - position);
//     let right = normalize(cross(cup, forward));
//     let up = cross(forward, right);

//     return mat4x4<f32>(
//         vec4<f32>(right.x, up.x, -forward.x, 0.0),
//         vec4<f32>(right.y, up.y, -forward.y, 0.0),
//         vec4<f32>(right.z, up.z, -forward.z, 0.0),
//         vec4<f32>(
//             -dot(right, position),
//             -dot(up, position),
//             dot(forward, position),
//             1.0
//         )
//     );
// }

// fn perspective(fov: f32, aspect: f32, near: f32, far: f32) -> mat4x4<f32> {
//     let f = 1.0 / tan(fov / 2.0);
//     let nf = 1.0 / (near - far);

//     return mat4x4<f32>(
//         vec4<f32>(f / aspect, 0.0, 0.0, 0.0),
//         vec4<f32>(0.0, f, 0.0, 0.0),
//         vec4<f32>(0.0, 0.0, (far + near) * nf, -1.0),
//         vec4<f32>(0.0, 0.0, 2.0 * far * near * nf, 0.0)
//     );
// }

// fn lod_error_is_imperceptible(sphere_center: vec3<f32>, sphere_radius: f32, proj : mat4x4<f32>) -> bool {
//     let d2 = dot(sphere_center, sphere_center);
//     let r2 = sphere_radius * sphere_radius;
    
//     let sphere_diameter_uv = camera.fov * sphere_radius / sqrt(d2 - r2);
//     let view_size = f32(max(camera.width, camera.height));
//     let sphere_diameter_pixels = sphere_diameter_uv * view_size;

//     return sphere_diameter_pixels < 1.0;
// }

// @compute @workgroup_size(64, 1, 1)
// fn get_lod_clusters(@builtin(global_invocation_id) gid: vec3<u32>) {
//     let clusterData = clusterData[gid.x];
//     let instancesData = instancesData[clusterData.instanceIndex];

//     let center = instancesData.modelMatrix * clusterData.selfBounds.center;
//     let radius = clusterData.selfBounds.radius;
// }


@vertex
fn vertex_clusters(input: VertexInput, @builtin(instance_index) instance_index: u32) -> VertexOutput {
    // let view = look_at(camera.position, camera.position + camera.direction, camera.cameraUp);
    // let proj = perspective(camera.fov, camera.aspectRatio, 0.01, 4000.0);
    
    // TODO: fix
    let model = instancesData[0].modelMatrix;

    let world_position = model * vec4<f32>(input.position, 1.0);
    let view_position = uniforms.view * world_position;
    let clip_position = uniforms.proj * view_position;

    var output: VertexOutput;

    output.position = clip_position;
    output.normal = vec4<f32>(input.normal.xyz, 0.0);
    output.uv = vec4<f32>(input.uv.xy, 0.0, 0.0);

    output.color = randomColor(f32(instance_index));

    return output;
}

@fragment
fn fragment_clusters(input: VertexOutput) -> @location(0) vec4f {
    return vec4f(input.color.x, input.color.y, input.color.z, 1.0);
    // return vec4f(input.color.x, input.color.y, input.color.z, 1.0);
}

@vertex
fn vertex_lines(input: VertexInput, @builtin(instance_index) instance_index: u32) -> VertexOutput {
    // let view = look_at(camera.position, camera.position + camera.direction, camera.cameraUp);
    // let proj = perspective(camera.fov, camera.aspectRatio, 0.01, 4000.0);
    // let model = instancesData[clusterData[instance_index].instanceIndex].modelMatrix;

    let world_position = vec4<f32>(input.position, 1.0);
    let view_position = uniforms.view * world_position;
    let clip_position = uniforms.proj * view_position;

    var output: VertexOutput;

    output.position = clip_position;
    output.normal = vec4<f32>(0.0, 0.0, 0.0, 0.0);
    output.uv = vec4<f32>(0.0, 0.0, 0.0, 0.0);

    output.color = randomColor(f32(0));

    return output;
}


@fragment
fn fragment_lines(input: VertexOutput) -> @location(0) vec4f {
    return vec4f(input.color.x, input.color.y, input.color.z, 1.0);
}

