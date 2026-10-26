// Low-poly terrain: legacy face colours or rewrite vertex cover colours.
//
// The colour is flat per face and the normal is smooth per vertex, which is not
// a contradiction - a field is one material whatever the light does across it.
// Deriving the normal from screen-space derivatives instead was exactly flat by
// construction and reported every sliver as its own facet, which on a mesh with
// millions of small triangles reads as noise rather than as relief.

struct Uniforms {
  // Column-major view-projection. Transposing a Mat4f into pass uniforms is a
  // silent way to get a healthy cull and an empty frame.
  viewProjection : mat4x4<f32>,
  // Unit direction from the surface toward the sun.
  sunDirection : vec4<f32>,
  // Sky tint above, ground bounce below, ambient strength in w of sky.
  skyColour : vec4<f32>,
  groundColour : vec4<f32>,
  // x: wireframe, y: sea level, z: rewrite covers/original normals, w: unused.
  options : vec4<f32>,
};

@group(0) @binding(0) var<uniform> uniforms : Uniforms;

struct VertexOutput {
  @builtin(position) clipPosition : vec4<f32>,
  @location(0) worldPosition : vec3<f32>,
  // Flat, not interpolated. A vertex on a boundary between two materials can
  // only carry one colour, so interpolating blends every triangle into its
  // neighbours and the material stops reading as a material. Flat takes the
  // provoking vertex, which makes the face one colour - which is what a
  // flat-shaded low-poly surface wants anyway.
  @location(1) @interpolate(flat) colour : vec4<f32>,
  @location(2) normal : vec3<f32>,
  @location(3) coverColour : vec4<f32>,
};

@vertex
fn vs_main(
  @location(0) position : vec3<f32>,
  @location(1) colour : vec4<f32>,
  @location(2) normal : vec3<f32>) -> VertexOutput
{
  var output : VertexOutput;
  output.clipPosition = uniforms.viewProjection * vec4<f32>(position, 1.0);
  output.worldPosition = position;
  output.colour = colour;
  output.coverColour = colour;
  output.normal = normal;
  return output;
}

@fragment
fn fs_main(input : VertexOutput) -> @location(0) vec4<f32>
{
  var normal = normalize(input.normal);
  // The vertex normal is averaged over every face at that world position, so a
  // degenerate sliver cannot flip it; still force it upward, because the mesh
  // is drawn without culling and a riser seen from below is still ground.
  if (normal.y < 0.0 && uniforms.options.z < 0.5) {
    normal = -normal;
  }

  let sun = normalize(uniforms.sunDirection.xyz);
  let lambert = max(dot(normal, sun), 0.0);
  // Hemispheric ambient: sky from above, bounce from below. Cheap, and it
  // keeps shadowed faces readable instead of crushing them to black.
  let hemisphere = normal.y * 0.5 + 0.5;
  let ambient = mix(uniforms.groundColour.rgb, uniforms.skyColour.rgb, hemisphere) *
      uniforms.skyColour.w;
  let colour = select(input.colour.rgb, input.coverColour.rgb, uniforms.options.z > 0.5);
  let lit = colour * (ambient + vec3<f32>(lambert));
  return vec4<f32>(lit, 1.0);
}
