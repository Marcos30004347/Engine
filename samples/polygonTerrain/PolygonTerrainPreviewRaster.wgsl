struct RasterUniforms
{
  // x/y atlas dimensions, z output pass, w whether a mountain map is present.
  dimensions : vec4<u32>,
};

@group(0) @binding(0) var<uniform> u : RasterUniforms;
@group(0) @binding(1) var inputA : texture_2d<f32>;
@group(0) @binding(2) var inputB : texture_2d<f32>;
@group(0) @binding(3) var inputC : texture_2d<f32>;
@group(0) @binding(4) var inputD : texture_2d<f32>;
@group(0) @binding(5) var output0 : texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(6) var output1 : texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(7) var output2 : texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(8) var output3 : texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(9) var output4 : texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(10) var output5 : texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(11) var output6 : texture_storage_2d<rgba8unorm, write>;

fn atlas_coord(position : vec2<i32>) -> vec2<i32>
{
  let width = i32(u.dimensions.x);
  let height = i32(u.dimensions.y);
  var x = position.x;
  var y = position.y;
  if (y < 0)
  {
    y = -y - 1;
    x += width / 2;
  }
  else if (y >= height)
  {
    y = height * 2 - y - 1;
    x += width / 2;
  }
  x = x % width;
  if (x < 0) { x += width; }
  return vec2<i32>(x, clamp(y, 0, height - 1));
}

fn bytes(value : vec4<f32>) -> vec4<u32>
{
  return vec4<u32>(round(clamp(value, vec4<f32>(0.0), vec4<f32>(1.0)) * 255.0));
}

fn height_at(position : vec2<i32>) -> f32
{
  let packed = bytes(textureLoad(inputA, atlas_coord(position), 0));
  return f32(packed.x * 256u + packed.y) / 65535.0;
}

fn rgb(r : u32, g : u32, b : u32) -> vec4<f32>
{
  return vec4<f32>(f32(r), f32(g), f32(b), 255.0) / 255.0;
}

fn ramp(a : vec4<f32>, b : vec4<f32>, amount : f32) -> vec4<f32>
{
  return mix(a, b, clamp(amount, 0.0, 1.0));
}

fn relief(position : vec2<i32>, mountain : f32) -> f32
{
  let center = height_at(position);
  if (center <= 0.0) { return 1.0; }
  let left = height_at(position + vec2<i32>(-1, 0));
  let right = height_at(position + vec2<i32>(1, 0));
  let top = height_at(position + vec2<i32>(0, -1));
  let bottom = height_at(position + vec2<i32>(0, 1));
  let dx = right - left;
  let dy = bottom - top;
  let normal = normalize(vec3<f32>(-dx * 36.0, -dy * 36.0, 1.0));
  let lambert = max(dot(normal, normalize(vec3<f32>(-0.42, -0.56, 0.72))), 0.0);
  let curvature = center * 4.0 - left - right - top - bottom;
  let ridge = clamp(curvature * 18.0, 0.0, 0.26);
  let valley = clamp(-curvature * 22.0, 0.0, 0.22);
  return clamp(0.58 + lambert * 0.48 + ridge - valley + mountain * 0.12, 0.34, 1.48);
}

fn apply_shade(color : vec4<f32>, amount : f32) -> vec4<f32>
{
  return vec4<f32>(clamp(color.rgb * clamp(amount, 0.30, 1.55), vec3<f32>(0.0), vec3<f32>(1.0)), 1.0);
}

fn region_color(value : u32) -> vec4<f32>
{
  if (value == 0u) { return rgb(12u, 36u, 76u); }
  let hash = value * 0x9e3779b9u;
  return rgb(72u + (hash & 0x7fu), 72u + ((hash >> 8u) & 0x7fu), 72u + ((hash >> 16u) & 0x7fu));
}

fn material_color(value : u32) -> vec4<f32>
{
  switch value
  {
    case 0u: { return rgb(13u, 41u, 84u); }
    case 1u: { return rgb(34u, 94u, 142u); }
    case 2u: { return rgb(208u, 190u, 121u); }
    case 3u, 17u: { return rgb(98u, 156u, 72u); }
    case 4u: { return rgb(130u, 123u, 111u); }
    case 5u: { return rgb(232u, 235u, 230u); }
    case 6u: { return rgb(38u, 125u, 184u); }
    case 7u: { return rgb(87u, 124u, 73u); }
    case 8u: { return rgb(205u, 230u, 238u); }
    case 9u: { return rgb(160u, 171u, 141u); }
    case 10u: { return rgb(142u, 134u, 119u); }
    case 11u: { return rgb(88u, 84u, 78u); }
    case 12u: { return rgb(73u, 118u, 92u); }
    case 13u: { return rgb(125u, 148u, 81u); }
    case 14u: { return rgb(196u, 174u, 112u); }
    case 15u: { return rgb(45u, 117u, 75u); }
    case 16u: { return rgb(65u, 139u, 74u); }
    case 18u: { return rgb(32u, 128u, 63u); }
    case 19u: { return rgb(59u, 150u, 70u); }
    case 20u: { return rgb(213u, 188u, 114u); }
    default: { return rgb(240u, 80u, 120u); }
  }
}

fn terrain_form_color(form : u32, height : f32) -> vec4<f32>
{
  var color = rgb(240u, 80u, 120u);
  switch form
  {
    case 0u: { return rgb(13u, 41u, 84u); }
    case 1u: { color = rgb(116u, 178u, 78u); }
    case 2u: { color = rgb(50u, 142u, 126u); }
    case 3u: { color = rgb(132u, 148u, 79u); }
    case 4u: { color = rgb(126u, 112u, 98u); }
    case 5u: { color = rgb(218u, 221u, 216u); }
    default: {}
  }
  return vec4<f32>(clamp(color.rgb * (0.62 + height * 0.46), vec3<f32>(0.0), vec3<f32>(1.0)), 1.0);
}

fn temperature_color(t : f32) -> vec4<f32>
{
  if (t < 0.35) { return ramp(rgb(44u, 77u, 152u), rgb(126u, 202u, 218u), t / 0.35); }
  if (t < 0.68) { return ramp(rgb(126u, 202u, 218u), rgb(227u, 205u, 96u), (t - 0.35) / 0.33); }
  return ramp(rgb(227u, 205u, 96u), rgb(187u, 63u, 43u), (t - 0.68) / 0.32);
}

fn precipitation_color(t : f32) -> vec4<f32>
{
  if (t < 0.42) { return ramp(rgb(169u, 135u, 74u), rgb(125u, 159u, 83u), t / 0.42); }
  if (t < 0.74) { return ramp(rgb(125u, 159u, 83u), rgb(48u, 135u, 92u), (t - 0.42) / 0.32); }
  return ramp(rgb(48u, 135u, 92u), rgb(53u, 128u, 188u), (t - 0.74) / 0.26);
}

fn wind_color(t : f32) -> vec4<f32>
{
  if (t < 0.50) { return ramp(rgb(38u, 35u, 80u), rgb(45u, 131u, 157u), t / 0.50); }
  return ramp(rgb(45u, 131u, 157u), rgb(218u, 235u, 226u), (t - 0.50) / 0.50);
}

fn forest_color(t : f32) -> vec4<f32>
{
  if (t < 0.45) { return ramp(rgb(52u, 63u, 44u), rgb(82u, 126u, 63u), t / 0.45); }
  return ramp(rgb(82u, 126u, 63u), rgb(23u, 101u, 53u), (t - 0.45) / 0.55);
}

fn settlement_color(t : f32) -> vec4<f32>
{
  if (t < 0.40) { return ramp(rgb(42u, 38u, 50u), rgb(112u, 98u, 72u), t / 0.40); }
  if (t < 0.76) { return ramp(rgb(112u, 98u, 72u), rgb(219u, 180u, 89u), (t - 0.40) / 0.36); }
  return ramp(rgb(219u, 180u, 89u), rgb(255u, 238u, 171u), (t - 0.76) / 0.24);
}

fn continent_color(t : f32) -> vec4<f32>
{
  if (t < 0.45) { return ramp(rgb(11u, 35u, 79u), rgb(42u, 111u, 151u), t / 0.45); }
  if (t < 0.58) { return ramp(rgb(42u, 111u, 151u), rgb(206u, 190u, 121u), (t - 0.45) / 0.13); }
  return ramp(rgb(102u, 151u, 72u), rgb(111u, 91u, 67u), (t - 0.58) / 0.42);
}

fn tectonic_color(t : f32) -> vec4<f32>
{
  if (t < 0.50) { return ramp(rgb(35u, 33u, 64u), rgb(102u, 76u, 120u), t / 0.50); }
  return ramp(rgb(102u, 76u, 120u), rgb(238u, 187u, 104u), (t - 0.50) / 0.50);
}

fn mountain_color(t : f32) -> vec4<f32>
{
  if (t < 0.35) { return ramp(rgb(28u, 44u, 58u), rgb(82u, 112u, 79u), t / 0.35); }
  if (t < 0.72) { return ramp(rgb(82u, 112u, 79u), rgb(128u, 111u, 94u), (t - 0.35) / 0.37); }
  return ramp(rgb(128u, 111u, 94u), rgb(236u, 237u, 230u), (t - 0.72) / 0.28);
}

@compute @workgroup_size(8, 8, 1)
fn raster_main(@builtin(global_invocation_id) global_id : vec3<u32>)
{
  if (global_id.x >= u.dimensions.x || global_id.y >= u.dimensions.y) { return; }
  let p = vec2<i32>(global_id.xy);
  let a = bytes(textureLoad(inputA, p, 0));
  let b = bytes(textureLoad(inputB, p, 0));
  let c = bytes(textureLoad(inputC, p, 0));
  let d = bytes(textureLoad(inputD, p, 0));
  let height = f32(a.x * 256u + a.y) / 65535.0;
  let mountain = select(0.0, f32(d.y) / 255.0, u.dimensions.w != 0u);
  let shade = relief(p, mountain);

  if (u.dimensions.z == 0u)
  {
    textureStore(output0, p, apply_shade(vec4<f32>(height, height, height, 1.0), shade));
    var river_color = apply_shade(vec4<f32>(height, height, height, 1.0), shade * 0.58);
    if (a.z != 0u)
    {
      let strength = f32(a.z) / 255.0;
      let water = ramp(rgb(38u, 116u, 178u), rgb(120u, 205u, 244u), strength);
      river_color = ramp(river_color, water, 0.48 + strength * 0.52);
    }
    textureStore(output1, p, river_color);
    textureStore(output2, p, region_color(a.w));
    textureStore(output3, p, apply_shade(material_color(b.x), shade));
    textureStore(output4, p, terrain_form_color(b.y, height));
    textureStore(output5, p, temperature_color(f32(b.z) / 255.0));
    textureStore(output6, p, precipitation_color(f32(b.w) / 255.0));
  }
  else
  {
    textureStore(output0, p, wind_color(f32(c.x) / 255.0));
    textureStore(output1, p, forest_color(f32(c.y) / 255.0));
    textureStore(output2, p, settlement_color(f32(c.z) / 255.0));
    textureStore(output3, p, continent_color(f32(c.w) / 255.0));
    textureStore(output4, p, tectonic_color(f32(d.x) / 255.0));
    let mountain_base = select(rgb(13u, 41u, 84u), mountain_color(mountain), height > 0.0);
    textureStore(output5, p, apply_shade(mountain_base, shade));
    textureStore(output6, p, vec4<f32>(0.0, 0.0, 0.0, 1.0));
  }
}
