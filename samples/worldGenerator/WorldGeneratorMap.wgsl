#include "map_export_addressing.wgsl"

struct MapUniforms
{
  // world metres -> Vulkan NDC: xy scale, zw offset.
  transform : vec4<f32>,
  // framebuffer dimensions in pixels, then their reciprocals.
  viewport : vec4<f32>,
  // cell, river, minor contour, major contour.
  lineWidths0 : vec4<f32>,
  // coast, then reserved values.
  lineWidths1 : vec4<f32>,
  // sea level, relief, vertical exaggeration, metres per screen pixel.
  shading : vec4<f32>,
  // Unit direction from the surface toward the sun.
  light : vec4<f32>,
  // Map layer in x; remaining values reserved for renderer flags.
  options : vec4<u32>,
  // Page interior, gutter, maximum quadtree level, stored page side.
  paging : vec4<u32>,
};

@group(0) @binding(0) var<uniform> u : MapUniforms;

struct MaterialSample
{
  contentColour : u32,
  landformColour : u32,
  biomeColour : u32,
  biomeBlendColour : u32,
  regionColour : u32,
  surfaceClass : u32,
  biomeIds : u32,
  biomeWeights : u32,
};

// Page-table entries contain physical page index + 1. Zero is non-resident.
@group(0) @binding(1) var<storage, read> materialPageTable : array<u32>;
@group(0) @binding(2) var<storage, read> materialPages : array<MaterialSample>;

struct VertexInput
{
  @location(0) position : vec2<f32>,
  @location(1) contentColour : vec4<f32>,
  @location(2) landformColour : vec4<f32>,
  @location(3) upliftColour : vec4<f32>,
  @location(4) biomeColour : vec4<f32>,
  @location(5) regionColour : vec4<f32>,
  @location(6) elevationMetres : f32,
};

struct VertexOutput
{
  @builtin(position) position : vec4<f32>,
  @location(0) colour : vec4<f32>,
  @location(1) elevationMetres : f32,
  @location(2) worldPosition : vec2<f32>,
};

fn unpack_colour(value : u32) -> vec4<f32>
{
  return vec4<f32>(
      f32(value & 255u),
      f32((value >> 8u) & 255u),
      f32((value >> 16u) & 255u),
      f32((value >> 24u) & 255u)) / 255.0;
}

struct ResolvedPage
{
  physical : u32,
  level : u32,
  coordinate : vec2<u32>,
};

fn resolve_material_page(coordinate : vec2<f32>) -> ResolvedPage
{
  var level = u.paging.z;
  let dimension = 1u << level;
  var pageCoordinate = min(vec2<u32>(coordinate * f32(dimension)),
      vec2<u32>(dimension - 1u));
  var entry = 0u;
  if level <= u.options.w
  {
    entry = materialPageTable[
        map_export_page_table_base(level) +
        map_export_page_morton(pageCoordinate, level)];
  }
  if entry != 0u
  {
    return ResolvedPage(entry - 1u, level, pageCoordinate);
  }
  if level > 0u
  {
    level = level - 1u;
    pageCoordinate = pageCoordinate / vec2<u32>(2u);
    if level <= u.options.w
    {
      entry = materialPageTable[
          map_export_page_table_base(level) +
          map_export_page_morton(pageCoordinate, level)];
    }
    if entry != 0u
    {
      return ResolvedPage(entry - 1u, level, pageCoordinate);
    }
  }
  if level > 0u
  {
    level = level - 1u;
    pageCoordinate = pageCoordinate / vec2<u32>(2u);
    entry = materialPageTable[
        map_export_page_table_base(level) +
        map_export_page_morton(pageCoordinate, level)];
    if entry != 0u
    {
      return ResolvedPage(entry - 1u, level, pageCoordinate);
    }
  }
  // Root residency is a construction invariant. Returning physical page zero
  // keeps malformed diagnostic data bounded instead of indexing arbitrary
  // storage.
  return ResolvedPage(0u, 0u, vec2<u32>(0u));
}

fn material_texel(page : ResolvedPage, global : vec2<u32>) -> MaterialSample
{
  let pageOrigin = page.coordinate * u.paging.x;
  let local = vec2<u32>(vec2<i32>(global) - vec2<i32>(pageOrigin) +
      vec2<i32>(i32(u.paging.y)));
  let pageTexels = u.paging.w * u.paging.w;
  return materialPages[page.physical * pageTexels +
      local.y * u.paging.w + local.x];
}

fn biome_colour(id : u32) -> vec3<f32>
{
  switch id
  {
    case 0u: { return vec3<f32>(0.086, 0.184, 0.318); }
    case 1u: { return vec3<f32>(0.243, 0.435, 0.588); }
    case 2u: { return vec3<f32>(0.847, 0.796, 0.627); }
    case 3u: { return vec3<f32>(0.937, 0.957, 0.973); }
    case 4u: { return vec3<f32>(0.678, 0.694, 0.635); }
    case 5u: { return vec3<f32>(0.741, 0.714, 0.620); }
    case 6u: { return vec3<f32>(0.820, 0.804, 0.718); }
    case 7u: { return vec3<f32>(0.278, 0.400, 0.353); }
    case 8u: { return vec3<f32>(0.290, 0.475, 0.267); }
    case 9u: { return vec3<f32>(0.165, 0.373, 0.259); }
    case 10u: { return vec3<f32>(0.227, 0.365, 0.239); }
    case 11u: { return vec3<f32>(0.478, 0.557, 0.302); }
    case 12u: { return vec3<f32>(0.663, 0.694, 0.412); }
    case 13u: { return vec3<f32>(0.714, 0.663, 0.416); }
    case 14u: { return vec3<f32>(0.686, 0.639, 0.400); }
    case 15u: { return vec3<f32>(0.788, 0.722, 0.408); }
    case 16u: { return vec3<f32>(0.400, 0.561, 0.259); }
    case 17u: { return vec3<f32>(0.118, 0.396, 0.208); }
    case 18u: { return vec3<f32>(0.220, 0.475, 0.290); }
    case 19u: { return vec3<f32>(0.855, 0.769, 0.545); }
    case 20u: { return vec3<f32>(0.902, 0.800, 0.510); }
    case 21u: { return vec3<f32>(0.373, 0.478, 0.400); }
    case 22u: { return vec3<f32>(0.247, 0.412, 0.325); }
    case 23u: { return vec3<f32>(0.173, 0.365, 0.290); }
    case 24u: { return vec3<f32>(0.588, 0.639, 0.502); }
    case 25u: { return vec3<f32>(0.596, 0.580, 0.545); }
    default: { return vec3<f32>(1.0, 0.0, 1.0); }
  }
}

fn sparse_biome_colour(
    page : ResolvedPage, coordinate : vec2<f32>) -> vec4<f32>
{
  let side = u.paging.x * (1u << page.level);
  let maximum = f32(side - 1u);
  let field = clamp(coordinate * f32(side) - vec2<f32>(0.5),
      vec2<f32>(0.0), vec2<f32>(maximum));
  let low = vec2<u32>(floor(field));
  let high = min(low + vec2<u32>(1u), vec2<u32>(side - 1u));
  let fraction = field - vec2<f32>(low);
  var samples : array<MaterialSample, 4>;
  samples[0] = material_texel(page, low);
  samples[1] = material_texel(page, vec2<u32>(high.x, low.y));
  samples[2] = material_texel(page, vec2<u32>(low.x, high.y));
  samples[3] = material_texel(page, high);
  let spatial = array<f32, 4>(
      (1.0 - fraction.x) * (1.0 - fraction.y),
      fraction.x * (1.0 - fraction.y),
      (1.0 - fraction.x) * fraction.y,
      fraction.x * fraction.y);
  var landAmount = 0.0;
  for (var tap = 0u; tap < 4u; tap++)
  {
    landAmount += spatial[tap] * f32(samples[tap].surfaceClass);
  }
  let surfaceClass = select(0u, 1u, landAmount >= 0.5);

  var ids : array<u32, 16>;
  var scores : array<f32, 16>;
  var priorities : array<f32, 16>;
  var count = 0u;
  for (var tap = 0u; tap < 4u; tap++)
  {
    if samples[tap].surfaceClass != surfaceClass { continue; }
    for (var slot = 0u; slot < 4u; slot++)
    {
      let id = (samples[tap].biomeIds >> (slot * 8u)) & 255u;
      let storedWeight =
          (samples[tap].biomeWeights >> (slot * 8u)) & 255u;
      let contribution = spatial[tap] * f32(storedWeight) / 255.0;
      if contribution <= 0.0 { continue; }
      var found = count;
      for (var candidate = 0u; candidate < count; candidate++)
      {
        if ids[candidate] == id { found = candidate; break; }
      }
      if found == count
      {
        ids[count] = id;
        count++;
      }
      scores[found] += contribution;
      priorities[found] += contribution * f32(4u - slot);
    }
  }

  var used : array<u32, 16>;
  var colour = vec3<f32>(0.0);
  var total = 0.0;
  for (var outputSlot = 0u; outputSlot < 4u; outputSlot++)
  {
    var best = 16u;
    for (var candidate = 0u; candidate < count; candidate++)
    {
      if used[candidate] != 0u { continue; }
      if best == 16u || scores[candidate] > scores[best] ||
          (scores[candidate] == scores[best] &&
           (priorities[candidate] > priorities[best] ||
            (priorities[candidate] == priorities[best] &&
             ids[candidate] < ids[best])))
      {
        best = candidate;
      }
    }
    if best == 16u { break; }
    used[best] = 1u;
    colour += biome_colour(ids[best]) * scores[best];
    total += scores[best];
  }
  return vec4<f32>(colour / max(total, 0.000001), 1.0);
}

fn threshold_material(position : vec2<f32>, layer : u32) -> vec4<f32>
{
  let extent = max(1.0, u.light.w);
  let coordinate = clamp(position / extent, vec2<f32>(0.0), vec2<f32>(1.0));
  let page = resolve_material_page(coordinate);
  if layer == 4u { return sparse_biome_colour(page, coordinate); }
  let side = u.paging.x * (1u << page.level);
  let global = min(vec2<u32>(coordinate * f32(side)),
      vec2<u32>(side - 1u));
  let sample = material_texel(page, global);
  switch layer
  {
    case 0u: { return unpack_colour(sample.contentColour); }
    case 1u: { return unpack_colour(sample.landformColour); }
    case 6u: { return unpack_colour(sample.regionColour); }
    default: { return vec4<f32>(1.0, 0.0, 1.0, 1.0); }
  }
}

fn elevation_colour(elevationMetres : f32) -> vec4<f32>
{
  let relief = max(1.0, u.shading.y);
  let elevation = elevationMetres - u.shading.x;
  if elevation >= 0.0
  {
    let t = clamp(elevation / relief, 0.0, 1.0);
    if t <= 0.08 { return vec4<f32>(mix(vec3<f32>(0.451, 0.588, 0.412), vec3<f32>(0.573, 0.647, 0.412), t / 0.08), 1.0); }
    if t <= 0.22 { return vec4<f32>(mix(vec3<f32>(0.573, 0.647, 0.412), vec3<f32>(0.741, 0.729, 0.482), (t - 0.08) / 0.14), 1.0); }
    if t <= 0.42 { return vec4<f32>(mix(vec3<f32>(0.741, 0.729, 0.482), vec3<f32>(0.769, 0.663, 0.463), (t - 0.22) / 0.20), 1.0); }
    if t <= 0.62 { return vec4<f32>(mix(vec3<f32>(0.769, 0.663, 0.463), vec3<f32>(0.671, 0.545, 0.427), (t - 0.42) / 0.20), 1.0); }
    if t <= 0.80 { return vec4<f32>(mix(vec3<f32>(0.671, 0.545, 0.427), vec3<f32>(0.612, 0.588, 0.573), (t - 0.62) / 0.18), 1.0); }
    return vec4<f32>(mix(vec3<f32>(0.612, 0.588, 0.573), vec3<f32>(0.949, 0.957, 0.969), (t - 0.80) / 0.20), 1.0);
  }
  let t = clamp(1.0 + elevation / (relief * 0.35), 0.0, 1.0);
  if t <= 0.45 { return vec4<f32>(mix(vec3<f32>(0.043, 0.098, 0.196), vec3<f32>(0.098, 0.208, 0.361), t / 0.45), 1.0); }
  if t <= 0.80 { return vec4<f32>(mix(vec3<f32>(0.098, 0.208, 0.361), vec3<f32>(0.184, 0.353, 0.522), (t - 0.45) / 0.35), 1.0); }
  return vec4<f32>(mix(vec3<f32>(0.184, 0.353, 0.522), vec3<f32>(0.353, 0.545, 0.663), (t - 0.80) / 0.20), 1.0);
}

@vertex fn vs_main(input : VertexInput) -> VertexOutput
{
  var output : VertexOutput;
  output.position = vec4<f32>(
      input.position * u.transform.xy + u.transform.zw, 0.5, 1.0);
  var colour = input.contentColour;
  switch u.options.x
  {
    case 1u: { colour = input.landformColour; }
    case 3u: { colour = input.upliftColour; }
    case 4u: { colour = input.biomeColour; }
    case 6u: { colour = input.regionColour; }
    default: {}
  }
  output.colour = colour;
  output.elevationMetres = input.elevationMetres;
  output.worldPosition = input.position;
  return output;
}

@fragment fn fs_main(input : VertexOutput) -> @location(0) vec4<f32>
{
  if u.options.z == 0u &&
      (u.options.x == 0u || u.options.x == 1u ||
       u.options.x == 4u || u.options.x == 6u)
  {
    return threshold_material(input.worldPosition, u.options.x);
  }
  if u.options.x == 2u
  {
    return elevation_colour(input.elevationMetres);
  }
  if u.options.x == 5u
  {
    let colour = elevation_colour(input.elevationMetres);
    let metresPerPixel = max(0.0001, u.shading.w);
    let dzdx = dpdx(input.elevationMetres) / metresPerPixel;
    // Framebuffer y grows down while map/world y grows north.
    let dzdy = -dpdy(input.elevationMetres) / metresPerPixel;
    let normal = normalize(vec3<f32>(
        -dzdx * u.shading.z, -dzdy * u.shading.z, 1.0));
    let shade = 0.25 + 0.95 * max(dot(normal, u.light.xyz), 0.0);
    return vec4<f32>(clamp(colour.rgb * shade, vec3<f32>(0.0), vec3<f32>(1.0)), colour.a);
  }
  return input.colour;
}

struct LineVertexInput
{
  @location(0) startPosition : vec2<f32>,
  @location(1) endPosition : vec2<f32>,
  @location(2) colour : vec4<f32>,
  @location(3) widthFactor : f32,
  @location(4) style : u32,
  @builtin(vertex_index) vertexIndex : u32,
};

struct LineVertexOutput
{
  @builtin(position) position : vec4<f32>,
  @location(0) colour : vec4<f32>,
  // Pixel coordinates relative to the segment start: x runs along the line,
  // y runs across it. The fragment shader uses this for analytic round caps.
  @location(1) linePosition : vec2<f32>,
  @location(2) @interpolate(flat) halfWidth : f32,
  @location(3) @interpolate(flat) segmentLength : f32,
};

fn line_width(style : u32, factor : f32) -> f32
{
  var base = u.lineWidths0.x;
  switch style
  {
    case 1u: { base = u.lineWidths0.y; }
    case 2u: { base = u.lineWidths0.z; }
    case 3u: { base = u.lineWidths0.w; }
    case 4u: { base = u.lineWidths1.x; }
    default: {}
  }
  let scaled = base * factor;
  return select(scaled, max(1.0, scaled), style == 1u);
}

@vertex fn line_vs(input : LineVertexInput) -> LineVertexOutput
{
  let a = input.startPosition * u.transform.xy + u.transform.zw;
  let b = input.endPosition * u.transform.xy + u.transform.zw;
  let deltaPixels = (b - a) * u.viewport.xy * 0.5;
  let lengthPixels = length(deltaPixels);
  let direction = select(
      vec2<f32>(1.0, 0.0), deltaPixels / max(lengthPixels, 0.0001),
      lengthPixels > 0.0001);
  let normal = vec2<f32>(-direction.y, direction.x);
  let halfWidth = line_width(input.style, input.widthFactor) * 0.5;
  let extrusion = halfWidth + 1.0;

  var along = 0.0;
  var side = -1.0;
  switch input.vertexIndex % 6u
  {
    case 1u: { side = 1.0; }
    case 2u: { along = 1.0; }
    case 3u: { along = 1.0; }
    case 4u: { side = 1.0; }
    case 5u: { along = 1.0; side = 1.0; }
    default: {}
  }
  let capDirection = select(-1.0, 1.0, along > 0.5);
  let pixelOffset =
      direction * capDirection * extrusion + normal * side * extrusion;
  let ndcOffset = pixelOffset * 2.0 * u.viewport.zw;

  var output : LineVertexOutput;
  output.position = vec4<f32>(mix(a, b, along) + ndcOffset, 0.25, 1.0);
  output.colour = input.colour;
  output.linePosition = vec2<f32>(
      mix(-extrusion, lengthPixels + extrusion, along), side * extrusion);
  output.halfWidth = halfWidth;
  output.segmentLength = lengthPixels;
  return output;
}

@fragment fn line_fs(input : LineVertexOutput) -> @location(0) vec4<f32>
{
  let outsideAlong = max(
      max(-input.linePosition.x,
          input.linePosition.x - input.segmentLength),
      0.0);
  let distanceToLine = length(
      vec2<f32>(outsideAlong, input.linePosition.y));
  let coverage =
      1.0 - smoothstep(input.halfWidth, input.halfWidth + 1.0,
                distanceToLine);
  return vec4<f32>(input.colour.rgb, input.colour.a * coverage);
}
