struct SphereUniforms
{
  mvp : mat4x4<f32>, model : mat4x4<f32>, lightAndLighting : vec4<f32>, atlasInfo : vec4<f32>,
};

@group(0) @binding(0) var<uniform> u : SphereUniforms;
@group(0) @binding(1) var atlasTexture : texture_2d<f32>;
@group(0) @binding(2) var atlasSampler : sampler;
@group(0) @binding(3) var regionTexture : texture_2d<f32>;
@group(0) @binding(4) var regionSampler : sampler;

struct VertexInput
{
  @location(0) position : vec3<f32>, @location(1) uv : vec2<f32>, @location(2) normal : vec3<f32>, @location(3) color : vec4<f32>,
};

struct VertexOutput
{
  @builtin(position) position : vec4<f32>, @location(0) planetNormal : vec3<f32>, @location(1) normal : vec3<f32>, @location(2) color : vec4<f32>, @location(3) @interpolate(flat) layer : u32,
  @location(4) worldPosition : vec3<f32>, @location(5) elevation : f32,
};

fn sphere_uv(normal : vec3<f32>) -> vec2<f32>
{
  let pi = 3.14159265358979323846;
  let n = normalize(normal);
  return vec2<f32>((atan2(n.z, n.x) + pi) / (2.0 * pi), 0.5 - asin(clamp(n.y, -1.0, 1.0)) / pi);
}

@vertex fn vs_main(input : VertexInput) -> VertexOutput
{
  var output : VertexOutput;
  output.position = u.mvp * vec4<f32>(input.position, 1.0);
  output.planetNormal = normalize(input.normal);
  output.normal = normalize((u.model * vec4<f32>(input.normal, 0.0)).xyz);
  output.worldPosition = (u.model * vec4<f32>(input.position, 1.0)).xyz;
  output.color = input.color;
  output.layer = 0u;
  if (input.uv.x < -2.5)
  {
    output.layer = 3u;
  }
  else if (input.uv.x < -1.5)
  {
    output.layer = 2u;
  }
  else if (input.uv.x < -0.5)
  {
    output.layer = 1u;
  }
  output.elevation = select(max(input.uv.y, 0.0), 0.0, input.uv.x < 0.0);
  return output;
}

fn sample_planet_atlas(planetNormal : vec3<f32>) -> vec4<f32>
{
  let n = normalize(planetNormal);
  let uv = sphere_uv(n);
  let atlasColor = textureSampleLevel(atlasTexture, atlasSampler, uv, 0.0);
  let texel = 1.0 / vec2<f32>(textureDimensions(atlasTexture));
  let neighbourhood =
      (textureSampleLevel(atlasTexture, atlasSampler, uv + vec2<f32>(texel.x, 0.0), 0.0) +
       textureSampleLevel(atlasTexture, atlasSampler, uv - vec2<f32>(texel.x, 0.0), 0.0) +
       textureSampleLevel(atlasTexture, atlasSampler, uv + vec2<f32>(0.0, texel.y), 0.0) +
       textureSampleLevel(atlasTexture, atlasSampler, uv - vec2<f32>(0.0, texel.y), 0.0)) * 0.25;
  let detailedColor = vec4<f32>(clamp(atlasColor.rgb + (atlasColor.rgb - neighbourhood.rgb) * 0.28, vec3<f32>(0.0), vec3<f32>(1.0)), atlasColor.a);

  // Longitude is undefined at a pole. Average several meridians into a small
  // polar cap instead of letting atan2(0, 0) turn one texel/seam into a pinched
  // star. Outside that cap the original high-resolution atlas is untouched.
  let poleColor =
      (textureSampleLevel(atlasTexture, atlasSampler, vec2<f32>(0.125, uv.y), 0.0) +
       textureSampleLevel(atlasTexture, atlasSampler, vec2<f32>(0.375, uv.y), 0.0) +
       textureSampleLevel(atlasTexture, atlasSampler, vec2<f32>(0.625, uv.y), 0.0) +
       textureSampleLevel(atlasTexture, atlasSampler, vec2<f32>(0.875, uv.y), 0.0)) * 0.25;
  let poleBlend = smoothstep(0.925, 0.995, abs(n.y));
  return mix(detailedColor, poleColor, poleBlend);
}

@fragment fn fs_main(input : VertexOutput) -> @location(0) vec4<f32>
{
  // The terrain layer is the generated spherical dual mesh itself. Texture
  // atlases remain export/debug products and do not define globe geometry.
  let color = input.color;

  let radialNormal = normalize(input.normal);
  if (input.layer == 1u)
  {
    // The reference GL_LINES pass fades rivers towards the silhouette instead
    // of trying to represent screen-space width there.
    let facing = smoothstep(0.02, 0.34, max(radialNormal.z, 0.0));
    return vec4<f32>(color.rgb, color.a * facing);
  }
  if (input.layer == 2u)
  {
    if (u.atlasInfo.y < 0.5)
    {
      discard;
    }
    let light = normalize(u.lightAndLighting.xyz);
    let view = normalize(vec3<f32>(0.0, 0.0, 3.0) - input.worldPosition);
    let diffuse = max(dot(radialNormal, light), 0.0);
    let halfway = normalize(light + view);
    let specular = pow(max(dot(radialNormal, halfway), 0.0), 96.0);
    let fresnel = pow(1.0 - max(dot(radialNormal, view), 0.0), 3.0);
    let deepWater = vec3<f32>(0.047, 0.227, 0.431);
    let litWater = deepWater * (0.78 + diffuse * 0.30) + vec3<f32>(0.24, 0.48, 0.68) * specular * 0.58 + vec3<f32>(0.10, 0.30, 0.55) * fresnel * 0.24;
    return vec4<f32>(litWater, 0.58 + fresnel * 0.10);
  }
  if (input.layer == 3u)
  {
    if (u.atlasInfo.y < 0.5)
    {
      discard;
    }
    let view = normalize(vec3<f32>(0.0, 0.0, 3.0) - input.worldPosition);
    let rim = pow(1.0 - max(dot(radialNormal, view), 0.0), 3.5);
    if (rim < 0.025)
    {
      discard;
    }
    return vec4<f32>(vec3<f32>(0.20, 0.43, 0.78), rim * 0.42);
  }
  // Keep lighting primarily radial, as the reference globe does. A small
  // derivative-normal contribution reveals relief without exposing every
  // dual-mesh diagonal as a hard faceted triangle.
  var reliefNormal = normalize(cross(dpdx(input.worldPosition), dpdy(input.worldPosition)));
  reliefNormal = select(-reliefNormal, reliefNormal, dot(reliefNormal, radialNormal) >= 0.0);
  let n = normalize(mix(radialNormal, reliefNormal, 0.28));
  let light = normalize(u.lightAndLighting.xyz);
  let lambert = max(dot(n, light), 0.0);
  let shade = mix(1.0, 0.62 + lambert * 0.54, clamp(u.lightAndLighting.w, 0.0, 1.0));
  let view = normalize(vec3<f32>(0.0, 0.0, 3.0) - input.worldPosition);
  let atmosphereRim = pow(1.0 - max(dot(radialNormal, view), 0.0), 4.0);
  let litColor = color.rgb * shade + vec3<f32>(0.08, 0.19, 0.38) * atmosphereRim * 0.28;
  return vec4<f32>(litColor, color.a);
}
