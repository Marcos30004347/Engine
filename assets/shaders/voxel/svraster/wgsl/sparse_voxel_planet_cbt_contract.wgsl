// Shared storage-buffer ABI for the adaptive background planet. This file is
// a preprocessor include for the phase-2+ kernels, not a standalone shader.
const PLANET_CBT_NULL_SLOT: u32 = 0xffffffffu;
const PLANET_CBT_COMMAND_NONE: u32 = 0u;
const PLANET_CBT_COMMAND_SPLIT: u32 = 1u;
const PLANET_CBT_COMMAND_MERGE: u32 = 2u;
const PLANET_CBT_COMMAND_ACCEPTED: u32 = 4u;
const PLANET_CBT_COMMAND_REJECTED_CAPACITY: u32 = 8u;
const PLANET_CBT_LIFECYCLE_DISABLE_VISIBILITY_DECIMATION: u32 = 1u;
const PLANET_CBT_LIFECYCLE_DISABLE_MINIMUM_VISIBLE_DEPTH: u32 = 2u;
const PLANET_CBT_LIFECYCLE_DISABLE_ADAPTIVE_DRAW: u32 = 4u;
const PLANET_CBT_MINIMUM_VISIBLE_DEPTH: u32 = 7u;
const PLANET_CBT_RENDER_TARGET_DEPTH: u32 = 15u;
const PLANET_CBT_MAXIMUM_RENDER_SUBDIVISION_DEPTH: u32 = 8u;

struct PlanetCbtBisectorGPU {
  next: u32,
  previous: u32,
  twin: u32,
  command: atomic<u32>,
  rootHalfedge: u32,
  depth: u32,
  pathLow: u32,
  pathHigh: u32,
  allocation: array<u32, 4>,
}

struct PlanetCbtPointerGPU {
  activePoolSlot: u32,
  freePoolSlot: u32,
}

struct PlanetCbtCountersGPU {
  activeCount: u32,
  freeCount: u32,
  requestedAllocations: u32,
  overflowCount: u32,
  splitCount: u32,
  mergeCount: u32,
  unchangedCount: atomic<u32>,
  maximumDepth: u32,
}

struct PlanetCbtDrawIndirectGPU {
  vertexCount: u32,
  instanceCount: u32,
  firstVertex: u32,
  firstInstance: u32,
}

struct PlanetCbtDispatchIndirectGPU {
  groupCountX: u32,
  groupCountY: u32,
  groupCountZ: u32,
  itemCount: u32,
}

struct PlanetCbtDispatchesGPU {
  activeDispatch: PlanetCbtDispatchIndirectGPU,
  freeDispatch: PlanetCbtDispatchIndirectGPU,
  pointerCacheDispatch: PlanetCbtDispatchIndirectGPU,
}

struct PlanetCbtRootTriangleGPU {
  vertex0: vec4<f32>,
  vertex1: vec4<f32>,
  vertex2: vec4<f32>,
}

struct PlanetCbtRequestGPU {
  rootHalfedge: u32,
  depth: u32,
  pathLow: u32,
  pathHigh: u32,
  command: u32,
  reserved: u32,
}

struct PlanetCbtUpdateSettingsGPU {
  viewportFocal: vec4<f32>,
  cameraDirectionDistance: vec4<f32>,
  cameraRight: vec4<f32>,
  cameraUp: vec4<f32>,
  lodThresholds: vec4<f32>,
  heightMapInfo: vec4<f32>,
  requestCount: u32,
  enableProjectedLod: u32,
  maximumDepth: u32,
  updateRoundCount: u32,
  frameIndex: u32,
  minimumFreeSlots: u32,
  lifecycleFlags: u32,
  maximumAllocationsPerRound: u32,
  renderSubdivisionDepth: u32,
  reserved0: u32,
  reserved1: u32,
  reserved2: u32,
}

struct PlanetCbtRoundSettingsGPU {
  roundIndex: u32,
  reserved0: u32,
  reserved1: u32,
  reserved2: u32,
}

fn planetCbtPathBit(low: u32, high: u32, bit: u32) -> u32 {
  if (bit < 32u) {
    return (low >> bit) & 1u;
  }
  return (high >> (bit - 32u)) & 1u;
}

fn planetCbtSubdivideVertex(
    vertex0: vec3<f32>, vertex1: vec3<f32>, vertex2: vec3<f32>,
    child: u32, corner: u32) -> vec3<f32> {
  if (corner == 2u) {
    return (vertex0 + vertex1) * 0.5;
  }
  if (corner == 0u) {
    return select(vertex2, vertex0, child == 0u);
  }
  return select(vertex1, vertex2, child == 0u);
}

fn planetCbtTextureMip(depth: u32, mipCount: u32) -> u32 {
  if (mipCount <= 1u || depth + 2u >= mipCount) {
    return 0u;
  }
  return mipCount - depth - 3u;
}

fn planetCbtCubeAtlasTexel(
    directionInput: vec3<f32>, baseFaceResolution: u32,
    mipLevel: u32) -> vec2<i32> {
  let direction = normalize(directionInput);
  let absolute = abs(direction);
  var face = 0u;
  var u = 0.0;
  var v = 0.0;
  if (absolute.x >= absolute.y && absolute.x >= absolute.z) {
    if (direction.x >= 0.0) {
      face = 0u;
      u = -direction.z / absolute.x;
    } else {
      face = 1u;
      u = direction.z / absolute.x;
    }
    v = -direction.y / absolute.x;
  } else if (absolute.y >= absolute.z) {
    if (direction.y >= 0.0) {
      face = 2u;
      u = direction.x / absolute.y;
      v = direction.z / absolute.y;
    } else {
      face = 3u;
      u = direction.x / absolute.y;
      v = -direction.z / absolute.y;
    }
  } else {
    if (direction.z >= 0.0) {
      face = 4u;
      u = direction.x / absolute.z;
    } else {
      face = 5u;
      u = -direction.x / absolute.z;
    }
    v = -direction.y / absolute.z;
  }
  let side = max(baseFaceResolution >> mipLevel, 1u);
  let uv = clamp(vec2<f32>(u, v) * 0.5 + vec2<f32>(0.5),
      vec2<f32>(0.0), vec2<f32>(0.999999));
  let local = vec2<u32>(uv * f32(side));
  return vec2<i32>(i32((face % 3u) * side + local.x),
      i32((face / 3u) * side + local.y));
}
