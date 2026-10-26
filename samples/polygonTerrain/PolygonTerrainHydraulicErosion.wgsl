struct ErosionUniforms
{
  dimensions : vec4<u32>,
  water : vec4<f32>,
  sediment : vec4<f32>,
  terrain : vec4<f32>,
};

@group(0) @binding(0) var<uniform> u : ErosionUniforms;
@group(0) @binding(1) var inputState : texture_2d<f32>;
@group(0) @binding(2) var inputFlux : texture_2d<f32>;
@group(0) @binding(3) var climateTexture : texture_2d<f32>;
@group(0) @binding(4) var outputTexture : texture_storage_2d<rgba32float, write>;

fn atlas_coord(position : vec2<i32>) -> vec2<i32>
{
  let width = i32(u.dimensions.x);
  let height = i32(u.dimensions.y);
  var x = position.x;
  var y = position.y;
  // Equirectangular rows are only storage. Across a geographic pole latitude
  // reflects and longitude advances by 180 degrees; clamping Y creates a false
  // horizontal wall and angular flow artifacts around both poles.
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

fn state_at(position : vec2<i32>) -> vec4<f32>
{
  return textureLoad(inputState, atlas_coord(position), 0);
}

fn flux_at(position : vec2<i32>) -> vec4<f32>
{
  return textureLoad(inputFlux, atlas_coord(position), 0);
}

fn climate_at(position : vec2<i32>) -> vec4<f32>
{
  return textureLoad(climateTexture, atlas_coord(position), 0);
}

fn longitude_metric(y : i32) -> f32
{
  let latitude = (0.5 - (f32(y) + 0.5) / f32(u.dimensions.y)) * 3.14159265358979323846;
  return max(abs(cos(latitude)), 0.20);
}

@compute @workgroup_size(8, 8, 1)
fn flux_main(@builtin(global_invocation_id) globalId : vec3<u32>)
{
  if (globalId.x >= u.dimensions.x || globalId.y >= u.dimensions.y) { return; }
  let p = vec2<i32>(globalId.xy);
  let state = state_at(p);
  let climate = climate_at(p);
  if (climate.y < 0.5)
  {
    textureStore(outputTexture, p, vec4<f32>(0.0));
    return;
  }

  let dt = u.water.x;
  let rain = u.water.y * climate.x * dt;
  let availableWater = max(state.y + rain, 0.0);
  let surface = state.x + availableWater;
  let metricX = longitude_metric(p.y);
  let leftSurface = state_at(p + vec2<i32>(-1, 0)).x + state_at(p + vec2<i32>(-1, 0)).y;
  let rightSurface = state_at(p + vec2<i32>(1, 0)).x + state_at(p + vec2<i32>(1, 0)).y;
  let topSurface = state_at(p + vec2<i32>(0, -1)).x + state_at(p + vec2<i32>(0, -1)).y;
  let bottomSurface = state_at(p + vec2<i32>(0, 1)).x + state_at(p + vec2<i32>(0, 1)).y;
  let oldFlux = max(flux_at(p), vec4<f32>(0.0));
  var flux = oldFlux + vec4<f32>(
      max(surface - leftSurface, 0.0) / metricX,
      max(surface - rightSurface, 0.0) / metricX,
      select(max(surface - topSurface, 0.0), 0.0, p.y == 0),
      select(max(surface - bottomSurface, 0.0), 0.0, p.y == i32(u.dimensions.y) - 1)) * (u.water.w * dt);
  let sumFlux = dot(flux, vec4<f32>(1.0));
  let maximumFlux = availableWater / max(dt, 0.00001);
  if (sumFlux > maximumFlux)
  {
    flux *= maximumFlux / max(sumFlux, 0.00001);
  }
  textureStore(outputTexture, p, max(flux, vec4<f32>(0.0)));
}

@compute @workgroup_size(8, 8, 1)
fn hydraulic_main(@builtin(global_invocation_id) globalId : vec3<u32>)
{
  if (globalId.x >= u.dimensions.x || globalId.y >= u.dimensions.y) { return; }
  let p = vec2<i32>(globalId.xy);
  let state = state_at(p);
  let climate = climate_at(p);
  if (climate.y < 0.5)
  {
    textureStore(outputTexture, p, vec4<f32>(state.x, 0.0, 0.0, 0.0));
    return;
  }

  let dt = u.water.x;
  let rain = u.water.y * climate.x * dt;
  let flux = max(flux_at(p), vec4<f32>(0.0));
  let incoming = vec4<f32>(
      flux_at(p + vec2<i32>(-1, 0)).y,
      flux_at(p + vec2<i32>(1, 0)).x,
      select(flux_at(p + vec2<i32>(0, -1)).w, 0.0, p.y == 0),
      select(flux_at(p + vec2<i32>(0, 1)).z, 0.0, p.y == i32(u.dimensions.y) - 1));
  let outgoingAmount = dot(flux, vec4<f32>(1.0));
  let incomingAmount = dot(incoming, vec4<f32>(1.0));
  var water = max(0.0, state.y + rain + dt * (incomingAmount - outgoingAmount));
  // Integrate through-flow over the whole erosion run. The old river output
  // sampled one final flux frame, which made streams sparse and unstable.
  let discharge = state.w + dt * (rain + 0.5 * (incomingAmount + outgoingAmount));

  let metricX = longitude_metric(p.y);
  let velocity = vec2<f32>(flux.y - flux.x, flux.w - flux.z) / vec2<f32>(metricX, 1.0);
  let speed = length(velocity) / max(water + state.y, 0.00005);
  let left = state_at(p + vec2<i32>(-1, 0)).x;
  let right = state_at(p + vec2<i32>(1, 0)).x;
  let top = state_at(p + vec2<i32>(0, -1)).x;
  let bottom = state_at(p + vec2<i32>(0, 1)).x;
  let slope = length(vec2<f32>((right - left) / (2.0 * metricX), (bottom - top) * 0.5));
  let capacity = max(slope, u.sediment.w) * speed * max(water, 0.00005) * u.sediment.x;

  var terrainHeight = state.x;
  var carriedSediment = max(state.z, 0.0);
  if (carriedSediment > capacity)
  {
    let deposited = (carriedSediment - capacity) * u.sediment.y * dt;
    terrainHeight += deposited;
    carriedSediment -= deposited;
  }
  else
  {
    let resistance = clamp(1.0 - climate.w * 0.82, 0.08, 1.0);
    let eroded = min((capacity - carriedSediment) * u.sediment.z * resistance * dt, max(terrainHeight - 0.00001, 0.0));
    terrainHeight -= eroded;
    carriedSediment += eroded;
  }
  water *= max(0.0, 1.0 - u.water.z * dt);
  textureStore(outputTexture, p, vec4<f32>(clamp(terrainHeight, 0.0, 1.0), water, max(carriedSediment, 0.0), discharge));
}

fn transported_sediment(position : vec2<i32>, direction : u32) -> f32
{
  let state = state_at(position);
  let flux = max(flux_at(position), vec4<f32>(0.0));
  let amount = select(select(flux.x, flux.y, direction == 1u), select(flux.z, flux.w, direction == 3u), direction >= 2u);
  return state.z * clamp(u.water.x * amount / max(state.y, 0.00005), 0.0, 1.0);
}

@compute @workgroup_size(8, 8, 1)
fn transport_main(@builtin(global_invocation_id) globalId : vec3<u32>)
{
  if (globalId.x >= u.dimensions.x || globalId.y >= u.dimensions.y) { return; }
  let p = vec2<i32>(globalId.xy);
  let state = state_at(p);
  if (climate_at(p).y < 0.5)
  {
    textureStore(outputTexture, p, vec4<f32>(state.x, 0.0, 0.0, state.w));
    return;
  }

  let flux = max(flux_at(p), vec4<f32>(0.0));
  let movedFraction = clamp(u.water.x * dot(flux, vec4<f32>(1.0)) / max(state.y, 0.00005), 0.0, 1.0);
  var incoming = transported_sediment(p + vec2<i32>(-1, 0), 1u) + transported_sediment(p + vec2<i32>(1, 0), 0u);
  if (p.y > 0) { incoming += transported_sediment(p + vec2<i32>(0, -1), 3u); }
  if (p.y < i32(u.dimensions.y) - 1) { incoming += transported_sediment(p + vec2<i32>(0, 1), 2u); }
  let sediment = max(0.0, state.z * (1.0 - movedFraction) + incoming);
  textureStore(outputTexture, p, vec4<f32>(state.x, state.y, sediment, state.w));
}
