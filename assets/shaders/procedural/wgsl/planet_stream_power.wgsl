// Stream power erosion over the six cube faces of a planet.
//
// This is Schott et al.'s GPU stream power solver, ported from
// StreamPowerErosion/data/shaders/spe_shader.glsl, with four deliberate
// departures from that reference:
//
//   1. The domain is six cube faces, not one square patch. Neighbour lookups
//      cross onto the adjacent face through the exact cube transform, so there
//      is no domain edge, no polar singularity, and no cos(latitude) metric.
//   2. Elevation and drainage live in f32 storage buffers. The WebGPU port of
//      this solver stores elevation in rgba8unorm; 256 height levels planet-wide
//      is not usable.
//   3. The reference's single kernel is split in three. Its GetFlowSteepest
//      writes each neighbour's flow direction into the *current* cell's slot,
//      so its own read path is disabled behind `if (false)`; computing receivers
//      in their own kernel fixes that race and removes eight redundant
//      recomputations per cell.
//   4. Ocean cells are the outlet boundary condition, replacing the reference's
//      zero-clamped domain border. On a closed sphere there is no border, so
//      the ocean mask is the only place elevation may be pinned.
//
// Per-cell state is one struct rather than a buffer per field, mirroring the
// reference's own vec4 Read/Write pair. Two ping-pong state buffers and one
// receiver buffer are the entire binding set, which keeps each cell to a single
// contiguous fetch and the descriptor count to four.
//
// Determinism: every kernel writes only its own cell, the drainage gather sums
// at most eight values in a fixed neighbour order, and there are no atomics.
// Two runs over the same input therefore produce bit-identical output, which is
// what the planet package's content hash depends on.

struct StreamPowerUniforms
{
  // side is cells per face edge; cellCount is 6 * side * side.
  side : u32,
  cellCount : u32,
  erosionMode : u32,
  reserved : u32,
  radiusMetres : f32,
  timeStep : f32,
  upliftRate : f32,
  streamPowerK : f32,
  hillslopeK : f32,
  debrisK : f32,
  areaExponent : f32,
  slopeExponent : f32,
  maximumElevationMetres : f32,
  sourceAreaScale : f32,
  // Lowest elevation a land cell may hold. The generator's height encoding
  // reserves everything below a small pedestal for sea, so a channel eroded
  // past it decodes to zero and material classification turns that land into
  // ocean. Clamping here keeps erosion from redrawing the authored coastline.
  minimumLandElevationMetres : f32,
  stratumThicknessMetres : f32,
  // Phase 3: soil production and sediment transport.
  weatheringRate : f32,
  weatheringDecayMetres : f32,
  transportK : f32,
  depositionK : f32,
  // Total land area. No catchment can exceed it, and without the bound the
  // carried accumulation can compound: phase 1's scheme relies on the surface
  // decreasing strictly along every receiver, which deposition breaks. When a
  // filled cell rises above its former donor the flow direction flips between
  // iterations, and a value that already contains a region's drainage can be
  // fed back into that region.
  maximumDrainageSquareMetres : f32,
  // Non-zero selects the transport-limited path: weathering, sediment routing
  // and deposition. Zero keeps the detachment-limited stream power of phase 1,
  // which is the calibrated default.
  sedimentTransport : u32,
  // Erodibility multiplier per PlanetLithology, two vec4s for the eight rock
  // types. Uniform arrays must be vec4-aligned, so this is packed rather than a
  // plain array<f32, 8>.
  erodibility : array<vec4<f32>, 2>,
};

// 24 bytes. The surface is bedrock plus regolith, and the two are tracked
// separately because that is what makes mass conserve: stream power on its own
// is detachment-limited and eroded rock simply leaves the world, which is why
// the reference solver produces no fans, no valley fill and no deltas.
struct PlanetCell
{
  // Bedrock surface, in metres. The regolith sits on top of it.
  bedrock : f32,
  // Regolith thickness in metres: weathered rock plus deposited sediment.
  regolith : f32,
  // Catchment area in square metres.
  drainage : f32,
  // Sediment flux leaving this cell, in cubic metres per unit time. Gathered
  // downstream exactly the way drainage is.
  sediment : f32,
  uplift : f32,
  // Rock the monotonicity clamp had to invent, accumulated in metres. It is
  // tracked rather than hidden: the clamp is the one place this solver can
  // create mass, and an untracked source is exactly the leak that a render
  // cannot show and a long run cannot survive.
  injected : f32,
  flags : u32,
};

fn surface_of(cell : PlanetCell) -> f32
{
  return cell.bedrock + cell.regolith;
}

const kOceanFlag : u32 = 1u;
// Rock suite occupies bits 8..11, and the bedding phase bits 16..23.
const kSuiteShift : u32 = 8u;
const kSuiteMask : u32 = 0xfu;
const kBedPhaseShift : u32 = 16u;
const kBedPhaseMask : u32 = 0xffu;

// Which bed of the sequence sits at this height. Mirrors
// planetStratumLithology in PlanetGeology.hpp; the two must agree, because the
// CPU resolves the same query for the exported material map and, later, for
// what a dug voxel column contains.
//
// Evaluating against the *current* height rather than a baked rock index is
// what makes layered ground behave like layered ground: as a channel incises it
// exposes the bed below, and a resistant bed becomes a bench where the surface
// arrives at it.
fn stratum_rock(suite : u32, elevation : f32, bedPhase : f32) -> u32
{
  let section = elevation + bedPhase * u.stratumThicknessMetres;
  let bed = i32(floor(section / u.stratumThicknessMetres));
  let index = u32(((bed % 4) + 4) % 4);
  switch (suite)
  {
    case 0u: {  // Oceanic: basalt with volcanic centres.
      if (index == 0u) { return 4u; }
      return 5u;
    }
    case 1u: {  // Basement: granite cores in metamorphic envelopes.
      if (index == 2u) { return 6u; }
      return 7u;
    }
    case 3u: {  // Volcanic province: weak ash between welded flows.
      if (index == 1u) { return 4u; }
      return 5u;
    }
    default: { break; }
  }
  // Platform: shale, sandstone caprock, limestone, mixed sediment.
  if (index == 0u) { return 0u; }
  if (index == 1u) { return 3u; }
  if (index == 2u) { return 2u; }
  return 1u;
}

// Erodibility of the rock currently exposed at a cell. Differential erosion is
// the entire point: uniform k cannot produce cliffs, benches, caprock or mesas
// at any iteration count, because each of those is one rock outlasting another.
fn cell_erodibility(flags : u32, elevation : f32) -> f32
{
  let suite = (flags >> kSuiteShift) & kSuiteMask;
  let bedPhase = f32((flags >> kBedPhaseShift) & kBedPhaseMask) / 255.0;
  let rock = stratum_rock(suite, elevation, bedPhase);
  let group = u.erodibility[rock / 4u];
  let lane = rock % 4u;
  if (lane == 0u) { return group.x; }
  if (lane == 1u) { return group.y; }
  if (lane == 2u) { return group.z; }
  return group.w;
}

@group(0) @binding(0) var<uniform> u : StreamPowerUniforms;
@group(0) @binding(1) var<storage, read> stateIn : array<PlanetCell>;
@group(0) @binding(2) var<storage, read_write> stateOut : array<PlanetCell>;
@group(0) @binding(3) var<storage, read_write> receiver : array<u32>;

// Direction index 8 means "no receiver": a cell with no lower neighbour.
const kNoReceiver : u32 = 8u;

// The reference's neighbour order, preserved exactly. Determinism of the
// drainage gather depends only on this order being fixed, but keeping the
// reference's makes the two implementations comparable term by term.
const kNeighbours = array<vec2<i32>, 8>(
    vec2<i32>(0, 1), vec2<i32>(1, 1), vec2<i32>(1, 0), vec2<i32>(1, -1),
    vec2<i32>(0, -1), vec2<i32>(-1, -1), vec2<i32>(-1, 0), vec2<i32>(-1, 1));

fn cell_index(face : u32, x : u32, y : u32) -> u32
{
  return (face * u.side + y) * u.side + x;
}

// Cell-centre direction. Accepts out-of-range coordinates: a cell centre just
// beyond a face edge is a real direction that lies on the adjacent face, which
// is what makes the seam transform exact rather than a copy.
fn cube_direction(face : u32, x : i32, y : i32) -> vec3<f32>
{
  let side = f32(u.side);
  let s = (f32(x) + 0.5) / side * 2.0 - 1.0;
  let t = (f32(y) + 0.5) / side * 2.0 - 1.0;
  var direction : vec3<f32>;
  switch (face)
  {
    case 0u: { direction = vec3<f32>(1.0, -t, -s); }
    case 1u: { direction = vec3<f32>(-1.0, -t, s); }
    case 2u: { direction = vec3<f32>(s, 1.0, t); }
    case 3u: { direction = vec3<f32>(s, -1.0, -t); }
    case 4u: { direction = vec3<f32>(s, -t, 1.0); }
    default: { direction = vec3<f32>(-s, -t, -1.0); }
  }
  return normalize(direction);
}

struct CubeCell
{
  index : u32,
  direction : vec3<f32>,
};

// Resolves a neighbour offset to a real cell, crossing faces where needed.
// Mirrors planetCubeFieldNeighbour and planetCubeCoordinate on the CPU side;
// the two must agree, because the CPU projects the field in and out.
fn neighbour_cell(face : u32, x : i32, y : i32) -> CubeCell
{
  let limit = i32(u.side);
  var result : CubeCell;
  if (x >= 0 && y >= 0 && x < limit && y < limit)
  {
    result.index = cell_index(face, u32(x), u32(y));
    result.direction = cube_direction(face, x, y);
    return result;
  }
  let direction = cube_direction(face, x, y);
  let magnitude = abs(direction);
  var targetFace : u32 = 0u;
  var s : f32 = 0.0;
  var t : f32 = 0.0;
  if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z)
  {
    if (direction.x >= 0.0)
    {
      targetFace = 0u;
      s = -direction.z / magnitude.x;
    }
    else
    {
      targetFace = 1u;
      s = direction.z / magnitude.x;
    }
    t = -direction.y / magnitude.x;
  }
  else if (magnitude.y >= magnitude.z)
  {
    if (direction.y >= 0.0)
    {
      targetFace = 2u;
      s = direction.x / magnitude.y;
      t = direction.z / magnitude.y;
    }
    else
    {
      targetFace = 3u;
      s = direction.x / magnitude.y;
      t = -direction.z / magnitude.y;
    }
  }
  else
  {
    if (direction.z >= 0.0)
    {
      targetFace = 4u;
      s = direction.x / magnitude.z;
    }
    else
    {
      targetFace = 5u;
      s = -direction.x / magnitude.z;
    }
    t = -direction.y / magnitude.z;
  }
  let side = f32(u.side);
  let targetX = clamp(i32(floor((s * 0.5 + 0.5) * side)), 0, limit - 1);
  let targetY = clamp(i32(floor((t * 0.5 + 0.5) * side)), 0, limit - 1);
  result.index = cell_index(targetFace, u32(targetX), u32(targetY));
  result.direction = direction;
  return result;
}

// Ground distance between two cell centres. The chord underestimates the arc
// by theta^2/24, which at these cell angles is below one part in a million, and
// unlike an analytic cube metric it stays correct across a face seam where the
// two cells belong to different tangent planes.
fn ground_distance(origin : vec3<f32>, destination : vec3<f32>) -> f32
{
  return max(length(origin - destination) * u.radiusMetres, 1.0e-3);
}

fn is_ocean(index : u32) -> bool
{
  return (stateIn[index].flags & kOceanFlag) != 0u;
}

// Ground area of one cell, in square metres.
//
// Drainage area is accumulated in square metres rather than in cells so that k
// means the same thing at every face resolution: a cell count would make the
// same landscape need a different k each time the bake size changed. Cube-sphere
// cells also vary about twofold in size from face centre to corner, which a
// constant per-cell seed would fold into the erodibility.
fn cell_area(face : u32, x : i32, y : i32) -> f32
{
  let east = neighbour_cell(face, x + 1, y);
  let west = neighbour_cell(face, x - 1, y);
  let north = neighbour_cell(face, x, y - 1);
  let south = neighbour_cell(face, x, y + 1);
  let width = ground_distance(east.direction, west.direction) * 0.5;
  let height = ground_distance(north.direction, south.direction) * 0.5;
  return width * height;
}

struct Local
{
  valid : bool,
  face : u32,
  x : i32,
  y : i32,
  index : u32,
  direction : vec3<f32>,
};

fn resolve(id : vec3<u32>) -> Local
{
  var local : Local;
  local.valid = false;
  if (id.x >= u.side || id.y >= u.side * 6u)
  {
    return local;
  }
  local.face = id.y / u.side;
  local.x = i32(id.x);
  local.y = i32(id.y % u.side);
  local.index = cell_index(local.face, id.x, id.y % u.side);
  local.direction = cube_direction(local.face, local.x, local.y);
  local.valid = true;
  return local;
}

// One D8 steepest-descent receiver per cell, written by that cell alone.
@compute @workgroup_size(8, 8, 1)
fn receiver_main(@builtin(global_invocation_id) id : vec3<u32>)
{
  let local = resolve(id);
  if (!local.valid)
  {
    return;
  }
  if (is_ocean(local.index))
  {
    receiver[local.index] = kNoReceiver;
    return;
  }
  let elevation = surface_of(stateIn[local.index]);
  var steepest : f32 = 0.0;
  var direction : u32 = kNoReceiver;
  for (var index = 0u; index < 8u; index = index + 1u)
  {
    let offset = kNeighbours[index];
    let neighbour = neighbour_cell(local.face, local.x + offset.x, local.y + offset.y);
    if (neighbour.index == local.index)
    {
      continue;
    }
    let drop = (elevation - surface_of(stateIn[neighbour.index])) / ground_distance(local.direction, neighbour.direction);
    if (drop > steepest)
    {
      steepest = drop;
      direction = index;
    }
  }
  receiver[local.index] = direction;
}

// Drainage area by gather: sum the areas of the neighbours whose own receiver
// points back at me. No atomics, no sort, no graph reduction — and therefore no
// dependence on dispatch order.
//
// This propagates catchment information exactly one cell per iteration, which
// is why the per-iteration cost is trivial and the iteration count is not.
@compute @workgroup_size(8, 8, 1)
fn accumulate_main(@builtin(global_invocation_id) id : vec3<u32>)
{
  let local = resolve(id);
  if (!local.valid)
  {
    return;
  }
  // Carrying the whole cell forward is what keeps the static fields alive
  // across the ping-pong; incise_main then overwrites elevation alone.
  var cell = stateIn[local.index];
  if ((cell.flags & kOceanFlag) != 0u)
  {
    // The sea is the outlet. Seeding it with zero keeps ocean cells from
    // feeding area back onto the land they drain.
    cell.drainage = 0.0;
    // Everything delivered to the sea leaves the model here. Reporting it is
    // what closes the mass balance: eroded rock either sits in regolith
    // somewhere or was exported through a coastal cell.
    cell.sediment = 0.0;
    stateOut[local.index] = cell;
    return;
  }
  var area = u.sourceAreaScale * cell_area(local.face, local.x, local.y);
  var load = 0.0;
  // Resolved neighbours, deduplicated. The cross-face transform is not
  // injective near a seam: two different offsets of the 3x3 stencil can land on
  // the same cell of the adjacent face, and at a cube corner one can resolve
  // back to this cell. Counting a donor twice compounds its catchment every
  // iteration, which showed up as a single basin claiming three times the
  // planet's entire land area.
  var seen = array<u32, 8>(0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u);
  var seenCount = 0u;
  for (var index = 0u; index < 8u; index = index + 1u)
  {
    let offset = kNeighbours[index];
    let neighbour = neighbour_cell(local.face, local.x + offset.x, local.y + offset.y);
    if (neighbour.index == local.index || is_ocean(neighbour.index))
    {
      continue;
    }
    var duplicate = false;
    for (var probe = 0u; probe < seenCount; probe = probe + 1u)
    {
      if (seen[probe] == neighbour.index)
      {
        duplicate = true;
      }
    }
    if (duplicate)
    {
      continue;
    }
    seen[seenCount] = neighbour.index;
    seenCount = seenCount + 1u;

    let neighbourReceiver = receiver[neighbour.index];
    if (neighbourReceiver == kNoReceiver)
    {
      continue;
    }
    // Does that neighbour's receiver resolve back to this cell?
    let cellsPerFace = u.side * u.side;
    let neighbourFace = neighbour.index / cellsPerFace;
    let within = neighbour.index % cellsPerFace;
    let neighbourX = i32(within % u.side);
    let neighbourY = i32(within / u.side);
    let step = kNeighbours[neighbourReceiver];
    let downstream = neighbour_cell(neighbourFace, neighbourX + step.x, neighbourY + step.y);
    if (downstream.index == local.index)
    {
      area = area + stateIn[neighbour.index].drainage;
      load = load + stateIn[neighbour.index].sediment;
    }
  }
  cell.drainage = min(area, u.maximumDrainageSquareMetres);
  cell.sediment = load;
  stateOut[local.index] = cell;
}

// Weathering, transport-limited erosion, deposition, then uplift.
//
// The transport-limited formulation is what phase 3 adds. Capacity is the
// stream power expression the detachment-limited model used as an erosion rate;
// here it is compared against the load actually arriving from upstream. Below
// capacity the flow entrains material, above it the flow deposits, and the
// difference is what puts alluvial fans at range fronts, fill in valley floors
// and deltas at river mouths.
@compute @workgroup_size(8, 8, 1)
fn incise_main(@builtin(global_invocation_id) id : vec3<u32>)
{
  let local = resolve(id);
  if (!local.valid)
  {
    return;
  }
  if (is_ocean(local.index))
  {
    // Fixed sea level: the boundary condition every catchment drains to.
    stateOut[local.index].bedrock = 0.0;
    stateOut[local.index].regolith = 0.0;
    return;
  }
  // Start from the cell accumulate_main just wrote, not from stateIn: it holds
  // this iteration's drainage and sediment as well as the carried-forward
  // bedrock, regolith and flags. Starting from stateIn instead silently threw
  // the fresh accumulation away every iteration and left drainage propagating
  // at half rate.
  var cell = stateOut[local.index];
  let elevation = surface_of(stateIn[local.index]);
  let direction = receiver[local.index];
  var receiverElevation = 0.0;
  var slope = 0.0;
  if (direction != kNoReceiver)
  {
    let offset = kNeighbours[direction];
    let neighbour = neighbour_cell(local.face, local.x + offset.x, local.y + offset.y);
    receiverElevation = surface_of(stateIn[neighbour.index]);
    slope = abs(elevation - receiverElevation) / ground_distance(local.direction, neighbour.direction);
  }

  let area = cell.drainage;
  var load = cell.sediment;
  let erodibility = cell_erodibility(cell.flags, cell.bedrock);

  if (u.sedimentTransport == 0u)
  {
    // Detachment-limited stream power: eroded rock leaves the system. No fans,
    // no valley fill, no deltas — but calibrated, and the regime the slope-area
    // gate was measured against.
    let incision = u.streamPowerK * erodibility * pow(max(area, 0.0), u.areaExponent) * pow(max(slope, 0.0), u.slopeExponent);
    var height = cell.bedrock - u.timeStep * incision;
    if (u.erosionMode >= 1u)
    {
      let east = neighbour_cell(local.face, local.x + 1, local.y);
      let west = neighbour_cell(local.face, local.x - 1, local.y);
      let north = neighbour_cell(local.face, local.x, local.y - 1);
      let south = neighbour_cell(local.face, local.x, local.y + 1);
      let dx = ground_distance(east.direction, west.direction) * 0.5;
      let dy = ground_distance(north.direction, south.direction) * 0.5;
      let laplacian =
          (surface_of(stateIn[east.index]) - 2.0 * elevation + surface_of(stateIn[west.index])) / (dx * dx) +
          (surface_of(stateIn[south.index]) - 2.0 * elevation + surface_of(stateIn[north.index])) / (dy * dy);
      height = height + u.timeStep * u.hillslopeK * laplacian;
    }
    height = max(height, receiverElevation);
    height = height + u.timeStep * u.upliftRate * cell.uplift;
    cell.bedrock = clamp(height, u.minimumLandElevationMetres, u.maximumElevationMetres);
    cell.regolith = 0.0;
    cell.sediment = 0.0;
    stateOut[local.index] = cell;
    return;
  }


  // Soil production: bedrock weathers into regolith, and thick soil armours the
  // rock beneath it. The exponential decay in existing depth is the standard
  // soil-production function and the reason soil depth self-limits rather than
  // growing without bound.
  let armour = exp(-cell.regolith / max(u.weatheringDecayMetres, 0.01));
  let production = u.weatheringRate * erodibility * armour * u.timeStep;
  cell.bedrock = cell.bedrock - production;
  cell.regolith = cell.regolith + production;

  // Transport capacity of the flow through this cell.
  let capacity = u.transportK * pow(max(area, 0.0), u.areaExponent) * pow(max(slope, 0.0), u.slopeExponent);
  var deposited = 0.0;
  if (load > capacity)
  {
    // Over capacity: drop the excess. Deposition is proportional to the excess
    // rather than instantaneous, so fills taper downstream instead of forming a
    // step at the first cell that cannot carry the load.
    deposited = (load - capacity) * u.depositionK * u.timeStep;
    deposited = min(deposited, load * u.timeStep);
    cell.regolith = cell.regolith + deposited;
    load = load - deposited / max(u.timeStep, 1.0e-6);
  }
  else
  {
    // Under capacity: entrain. Loose regolith goes first and bedrock only when
    // the regolith is exhausted, which is the transport-limited to
    // detachment-limited transition and the reason valley floors read as filled
    // rather than scoured.
    var demand = (capacity - load) * u.timeStep;
    let fromRegolith = min(demand, cell.regolith);
    cell.regolith = cell.regolith - fromRegolith;
    demand = demand - fromRegolith;
    let fromBedrock = demand * erodibility;
    cell.bedrock = cell.bedrock - fromBedrock;
    load = load + (fromRegolith + fromBedrock) / max(u.timeStep, 1.0e-6);
  }

  if (u.erosionMode >= 1u)
  {
    // Hillslope diffusion, applied to the regolith rather than to the rock:
    // creep moves soil, not bedrock.
    let east = neighbour_cell(local.face, local.x + 1, local.y);
    let west = neighbour_cell(local.face, local.x - 1, local.y);
    let north = neighbour_cell(local.face, local.x, local.y - 1);
    let south = neighbour_cell(local.face, local.x, local.y + 1);
    let dx = ground_distance(east.direction, west.direction) * 0.5;
    let dy = ground_distance(north.direction, south.direction) * 0.5;
    let laplacian =
        (surface_of(stateIn[east.index]) - 2.0 * elevation + surface_of(stateIn[west.index])) / (dx * dx) +
        (surface_of(stateIn[south.index]) - 2.0 * elevation + surface_of(stateIn[north.index])) / (dy * dy);
    // Creep can only move soil that exists; taking more would be a second
    // silent mass source.
    let creep = u.hillslopeK * laplacian * u.timeStep;
    cell.regolith = cell.regolith + max(creep, -cell.regolith);
  }

  cell.bedrock = cell.bedrock + u.timeStep * u.upliftRate * cell.uplift;
  // Regolith cannot go negative. Clipping is a mass source, so it is counted
  // with the rest rather than quietly absorbed.
  let clipped = max(cell.regolith, 0.0);
  cell.injected = cell.injected + (clipped - cell.regolith);
  cell.regolith = clipped;

  // Keep the surface at or above the cell it drains into, so depressions cannot
  // form and no depression-routing pass is ever needed.
  //
  // The reference does this by raising the rock outright, which invents mass.
  // Here the deficit is filled from the sediment actually passing through the
  // cell first — a depression filling with its own alluvium, which is what
  // happens — and only the part no sediment can cover is invented, and counted.
  let surface = cell.bedrock + cell.regolith;
  if (surface < receiverElevation)
  {
    let deficit = receiverElevation - surface;
    let available = load * u.timeStep;
    let filled = min(deficit, available);
    cell.regolith = cell.regolith + filled;
    load = load - filled / max(u.timeStep, 1.0e-6);
    let invented = deficit - filled;
    cell.bedrock = cell.bedrock + invented;
    cell.injected = cell.injected + invented;
  }
  // The elevation clamps are the other place the books move: the land pedestal
  // invents rock under a cell eroded below it, and the ceiling destroys rock
  // under a cell uplifted past it. The term is signed so both are counted.
  let floored = clamp(cell.bedrock, u.minimumLandElevationMetres, u.maximumElevationMetres);
  cell.injected = cell.injected + (floored - cell.bedrock);
  cell.bedrock = floored;
  cell.sediment = max(load, 0.0);
  stateOut[local.index] = cell;
}
