#include "sparse_voxel_planet_cbt_contract.wgsl"

@group(0) @binding(0) var<storage, read_write> bisectors: array<PlanetCbtBisectorGPU>;
@group(0) @binding(1) var<storage, read_write> cbtHeap: array<u32>;
@group(0) @binding(2) var<storage, read> pointerCache: array<PlanetCbtPointerGPU>;
@group(0) @binding(3) var<storage, read_write> counters: PlanetCbtCountersGPU;
@group(0) @binding(4) var<storage, read> dispatches: PlanetCbtDispatchesGPU;
@group(0) @binding(5) var<storage, read> roots: array<PlanetCbtRootTriangleGPU>;
@group(0) @binding(6) var<storage, read> requests: array<PlanetCbtRequestGPU>;
@group(0) @binding(7) var<storage, read_write> bitfieldChanges: array<u32>;
@group(0) @binding(8) var<uniform> settings: PlanetCbtUpdateSettingsGPU;
@group(0) @binding(9) var heightMinMax: texture_2d<f32>;
@group(0) @binding(10) var<uniform> roundSettings: PlanetCbtRoundSettingsGPU;
@group(0) @binding(11) var<storage, read_write> commandIndices: array<u32>;

const PLANET_CBT_BITFIELD_UNCHANGED = 0u;
const PLANET_CBT_BITFIELD_ACTIVATE = 1u;
const PLANET_CBT_BITFIELD_RELEASE = 2u;
const PLANET_CBT_MAXIMUM_COMPATIBILITY_CHAIN = 64u;

fn planetCbtRoundEnabled() -> bool {
  return roundSettings.roundIndex < settings.updateRoundCount;
}

fn validSlot(slot: u32) -> bool {
  return slot < arrayLength(&bisectors) &&
      bisectors[slot].rootHalfedge != PLANET_CBT_NULL_SLOT;
}

fn sameAddressRequest(slot: u32, request: PlanetCbtRequestGPU) -> bool {
  return bisectors[slot].rootHalfedge == request.rootHalfedge &&
      bisectors[slot].depth == request.depth &&
      bisectors[slot].pathLow == request.pathLow &&
      bisectors[slot].pathHigh == request.pathHigh;
}

fn childPathLow(low: u32, child: u32) -> u32 {
  return (low << 1u) | child;
}

fn childPathHigh(low: u32, high: u32) -> u32 {
  return (high << 1u) | (low >> 31u);
}

fn parentPathLow(low: u32, high: u32) -> u32 {
  return (low >> 1u) | (high << 31u);
}

fn parentPathHigh(high: u32) -> u32 {
  return high >> 1u;
}

fn addressesAreSiblings(first: u32, second: u32) -> bool {
  if (!validSlot(first) || !validSlot(second) ||
      bisectors[first].depth == 0u ||
      bisectors[first].rootHalfedge != bisectors[second].rootHalfedge ||
      bisectors[first].depth != bisectors[second].depth) {
    return false;
  }
  return parentPathLow(bisectors[first].pathLow, bisectors[first].pathHigh) ==
          parentPathLow(bisectors[second].pathLow, bisectors[second].pathHigh) &&
      parentPathHigh(bisectors[first].pathHigh) ==
          parentPathHigh(bisectors[second].pathHigh) &&
      (bisectors[first].pathLow & 1u) != (bisectors[second].pathLow & 1u);
}

fn replaceNeighbor(neighbor: u32, oldSlot: u32, newSlot: u32) {
  if (!validSlot(neighbor)) {
    return;
  }
  if (bisectors[neighbor].next == oldSlot) {
    bisectors[neighbor].next = newSlot;
  }
  if (bisectors[neighbor].previous == oldSlot) {
    bisectors[neighbor].previous = newSlot;
  }
  if (bisectors[neighbor].twin == oldSlot) {
    bisectors[neighbor].twin = newSlot;
  }
}

fn writeBisector(
    slot: u32, next: u32, previous: u32, twin: u32,
    rootHalfedge: u32, depth: u32, pathLowValue: u32,
    pathHighValue: u32) {
  bisectors[slot].next = next;
  bisectors[slot].previous = previous;
  bisectors[slot].twin = twin;
  atomicStore(&bisectors[slot].command, PLANET_CBT_COMMAND_ACCEPTED);
  bisectors[slot].rootHalfedge = rootHalfedge;
  bisectors[slot].depth = depth;
  bisectors[slot].pathLow = pathLowValue;
  bisectors[slot].pathHigh = pathHighValue;
  for (var index = 0u; index < 4u; index += 1u) {
    bisectors[slot].allocation[index] = PLANET_CBT_NULL_SLOT;
  }
}

// Reservation is serialized, so it can maintain the CBT sums along the
// changed leaf's ancestor path without a full bottom-up reduction. This turns
// the per-round O(capacity) rebuild into O(changes * treeDepth).
fn setHeapLeaf(slot: u32, isActive: bool) {
  var node = arrayLength(&bisectors) + slot;
  let previous = cbtHeap[node];
  let next = select(0u, 1u, isActive);
  if (previous == next) {
    return;
  }
  cbtHeap[node] = next;
  loop {
    node /= 2u;
    if (node == 0u) {
      break;
    }
    if (isActive) {
      cbtHeap[node] += 1u;
    } else {
      cbtHeap[node] -= 1u;
    }
  }
  cbtHeap[0u] = 0u;
}

fn splitCompatiblePair(
    first: u32, second: u32, firstRight: u32, secondRight: u32) {
  let firstNext = bisectors[first].next;
  let firstPrevious = bisectors[first].previous;
  let secondNext = bisectors[second].next;
  let secondPrevious = bisectors[second].previous;
  let firstRoot = bisectors[first].rootHalfedge;
  let firstDepth = bisectors[first].depth;
  let firstLow = bisectors[first].pathLow;
  let firstHigh = bisectors[first].pathHigh;
  let secondRoot = bisectors[second].rootHalfedge;
  let secondDepth = bisectors[second].depth;
  let secondLow = bisectors[second].pathLow;
  let secondHigh = bisectors[second].pathHigh;

  // Child zero reuses its parent slot. Only the external next-side neighbors
  // need a physical-pointer repair; previous-side neighbors still point at
  // the reused child-zero slots.
  replaceNeighbor(firstNext, first, firstRight);
  replaceNeighbor(secondNext, second, secondRight);

  writeBisector(first, firstRight, secondRight, firstPrevious,
      firstRoot, firstDepth + 1u,
      childPathLow(firstLow, 0u), childPathHigh(firstLow, firstHigh));
  writeBisector(firstRight, second, first, firstNext,
      firstRoot, firstDepth + 1u,
      childPathLow(firstLow, 1u), childPathHigh(firstLow, firstHigh));
  writeBisector(second, secondRight, firstRight, secondPrevious,
      secondRoot, secondDepth + 1u,
      childPathLow(secondLow, 0u), childPathHigh(secondLow, secondHigh));
  writeBisector(secondRight, first, second, secondNext,
      secondRoot, secondDepth + 1u,
      childPathLow(secondLow, 1u), childPathHigh(secondLow, secondHigh));
  bitfieldChanges[firstRight] = PLANET_CBT_BITFIELD_ACTIVATE;
  bitfieldChanges[secondRight] = PLANET_CBT_BITFIELD_ACTIVATE;
  setHeapLeaf(firstRight, true);
  setHeapLeaf(secondRight, true);
  counters.splitCount += 2u;
  counters.maximumDepth = max(counters.maximumDepth,
      max(firstDepth, secondDepth) + 1u);
}

fn markSplitCompatibilityChain(start: u32) {
  var current = start;
  for (var iteration = 0u;
       iteration < PLANET_CBT_MAXIMUM_COMPATIBILITY_CHAIN;
       iteration += 1u) {
    if (!validSlot(current)) {
      return;
    }
    atomicOr(&bisectors[current].command, PLANET_CBT_COMMAND_SPLIT);
    let neighbor = bisectors[current].twin;
    if (!validSlot(neighbor)) {
      return;
    }
    atomicOr(&bisectors[neighbor].command, PLANET_CBT_COMMAND_SPLIT);
    if (bisectors[neighbor].twin == current) {
      return;
    }
    current = neighbor;
  }
}

fn markMergeDiamond(slot: u32) {
  if (!validSlot(slot) || bisectors[slot].depth == 0u) {
    return;
  }
  var child0 = slot;
  var child1 = slot;
  if ((bisectors[slot].pathLow & 1u) == 0u) {
    child1 = bisectors[slot].next;
  } else {
    child0 = bisectors[slot].previous;
  }
  if (!addressesAreSiblings(child0, child1)) {
    return;
  }
  let opposite1 = bisectors[child0].previous;
  let opposite0 = bisectors[child1].next;
  if (!addressesAreSiblings(opposite0, opposite1)) {
    return;
  }
  atomicOr(&bisectors[child0].command, PLANET_CBT_COMMAND_MERGE);
  atomicOr(&bisectors[child1].command, PLANET_CBT_COMMAND_MERGE);
  atomicOr(&bisectors[opposite0].command, PLANET_CBT_COMMAND_MERGE);
  atomicOr(&bisectors[opposite1].command, PLANET_CBT_COMMAND_MERGE);
}

fn requestedCommand(slot: u32) -> u32 {
  var command = 0u;
  for (var requestIndex = 0u;
       requestIndex < settings.requestCount;
       requestIndex += 1u) {
    let request = requests[requestIndex];
    if (sameAddressRequest(slot, request)) {
      command |= request.command;
    }
  }
  return command;
}

fn decodeTriangle(slot: u32) -> array<vec3<f32>, 3> {
  let addressRoot = min(bisectors[slot].rootHalfedge,
      arrayLength(&roots) - 1u);
  let root = roots[addressRoot];
  var triangle = array<vec3<f32>, 3>(
      root.vertex0.xyz, root.vertex1.xyz, root.vertex2.xyz);
  let depth = bisectors[slot].depth;
  for (var level = 0u; level < depth; level += 1u) {
    let bit = planetCbtPathBit(bisectors[slot].pathLow,
        bisectors[slot].pathHigh, depth - level - 1u);
    let current = triangle;
    triangle = array<vec3<f32>, 3>(
        planetCbtSubdivideVertex(current[0], current[1], current[2], bit, 0u),
        planetCbtSubdivideVertex(current[0], current[1], current[2], bit, 1u),
        planetCbtSubdivideVertex(current[0], current[1], current[2], bit, 2u));
  }
  triangle[0] = normalize(triangle[0]);
  triangle[1] = normalize(triangle[1]);
  triangle[2] = normalize(triangle[2]);
  return triangle;
}

fn projectedLodError(slot: u32) -> f32 {
  let triangle = decodeTriangle(slot);
  let mipCount = max(u32(settings.heightMapInfo.y), 1u);
  let mip = planetCbtTextureMip(bisectors[slot].depth, mipCount);
  let faceResolution = max(u32(settings.heightMapInfo.x), 1u);
  let reliefScale = max(settings.cameraRight.w, 0.0);
  var displaced = triangle;
  var minimumHeight = 1.0;
  var maximumHeight = 0.0;
  for (var corner = 0u; corner < 3u; corner += 1u) {
    let texel = planetCbtCubeAtlasTexel(
        triangle[corner], faceResolution, mip);
    let range = textureLoad(heightMinMax, texel, i32(mip)).xy;
    minimumHeight = min(minimumHeight, range.x);
    maximumHeight = max(maximumHeight, range.y);
    let height = settings.heightMapInfo.z +
        ((range.x + range.y) * 0.5) * settings.heightMapInfo.w;
    displaced[corner] *= 1.0 + height * reliefScale;
  }
  let centreDirection = normalize(
      triangle[0] + triangle[1] + triangle[2]);
  let centreRange = textureLoad(heightMinMax,
      planetCbtCubeAtlasTexel(centreDirection, faceResolution, mip),
      i32(mip)).xy;
  minimumHeight = min(minimumHeight, centreRange.x);
  maximumHeight = max(maximumHeight, centreRange.y);
  let cameraDirection = normalize(settings.cameraDirectionDistance.xyz);
  let cameraPosition = cameraDirection * settings.cameraDirectionDistance.w;
  let cameraForward = -cameraDirection;
  let cameraRight = normalize(settings.cameraRight.xyz);
  let cameraUp = normalize(settings.cameraUp.xyz);
  let aspect = settings.viewportFocal.x / max(settings.viewportFocal.y, 1.0);
  var pixels = array<vec2<f32>, 3>();
  var minimumPixel = vec2<f32>(1.0e20);
  var maximumPixel = vec2<f32>(-1.0e20);
  var behindNearPlane = 0u;
  for (var corner = 0u; corner < 3u; corner += 1u) {
    let relative = displaced[corner] - cameraPosition;
    let view = vec3<f32>(dot(relative, cameraRight),
        -dot(relative, cameraUp), dot(relative, cameraForward));
    if (view.z <= settings.viewportFocal.w) {
      behindNearPlane += 1u;
      continue;
    }
    let ndc = vec2<f32>(
        view.x * settings.viewportFocal.z / (aspect * view.z),
        view.y * settings.viewportFocal.z / view.z);
    pixels[corner] = (ndc * 0.5 + vec2<f32>(0.5)) *
        settings.viewportFocal.xy;
    minimumPixel = min(minimumPixel, pixels[corner]);
    maximumPixel = max(maximumPixel, pixels[corner]);
  }
  let visibilityDecimation = (settings.lifecycleFlags &
      PLANET_CBT_LIFECYCLE_DISABLE_VISIBILITY_DECIMATION) == 0u;
  if (behindNearPlane == 3u && visibilityDecimation) {
    return 0.0;
  }
  if (behindNearPlane != 0u) {
    return settings.lodThresholds.x * settings.lodThresholds.y + 1.0;
  }
  let offscreenMargin = max(settings.lodThresholds.w, 0.0);
  if (visibilityDecimation && (maximumPixel.x < -offscreenMargin ||
      maximumPixel.y < -offscreenMargin ||
      minimumPixel.x > settings.viewportFocal.x + offscreenMargin ||
      minimumPixel.y > settings.viewportFocal.y + offscreenMargin)) {
    return 0.0;
  }
  var maximumFacing = -1.0;
  for (var corner = 0u; corner < 3u; corner += 1u) {
    maximumFacing = max(maximumFacing,
        dot(triangle[corner], cameraDirection));
  }
  if (visibilityDecimation &&
      maximumFacing * settings.cameraDirectionDistance.w <= 1.0) {
    return 0.0;
  }
  let first = pixels[1] - pixels[0];
  let second = pixels[2] - pixels[0];
  let area = abs(first.x * second.y - first.y * second.x) * 0.5;
  let centrePosition = (displaced[0] + displaced[1] + displaced[2]) / 3.0;
  let centreViewDepth = max(dot(centrePosition - cameraPosition,
      cameraForward), settings.viewportFocal.w);
  let pixelsPerRadius = settings.viewportFocal.y *
      settings.viewportFocal.z * 0.5 / centreViewDepth;
  let displacementRange = max(maximumHeight - minimumHeight, 0.0) *
      settings.heightMapInfo.w * reliefScale;
  let displacementError = displacementRange * pixelsPerRadius;
  let edgeSquared = max(dot(triangle[1] - triangle[0],
      triangle[1] - triangle[0]), max(dot(triangle[2] - triangle[1],
      triangle[2] - triangle[1]), dot(triangle[0] - triangle[2],
      triangle[0] - triangle[2])));
  let silhouetteSagitta = 1.0 - sqrt(max(1.0 - edgeSquared * 0.25, 0.0));
  let silhouetteError = silhouetteSagitta * pixelsPerRadius;
  return max(area, max(displacementError * displacementError,
      silhouetteError * silhouetteError));
}

fn generatedCommand(slot: u32) -> u32 {
  var command = requestedCommand(slot);
  if (settings.enableProjectedLod == 0u) {
    return command;
  }
  let error = projectedLodError(slot);
  let depth = bisectors[slot].depth;
  var maximumDepth = settings.maximumDepth;
  if (settings.heightMapInfo.x > 0.0) {
    maximumDepth = min(maximumDepth,
        u32(settings.heightMapInfo.y) + 4u);
  }
  let minimumVisibleDepthEnabled = (settings.lifecycleFlags &
      PLANET_CBT_LIFECYCLE_DISABLE_MINIMUM_VISIBLE_DEPTH) == 0u;
  let minimumVisibleDepth = min(
      PLANET_CBT_MINIMUM_VISIBLE_DEPTH, maximumDepth);
  // projectedLodError returns zero only for geometry rejected by visibility
  // decimation. A positive value, however small, identifies a visible leaf.
  // Keep those leaves above the silhouette floor independently of projected
  // area; hidden/off-screen leaves can still collapse to roots.
  let visible = error > 0.0;
  let belowVisibleFloor = minimumVisibleDepthEnabled && visible &&
      depth < minimumVisibleDepth;
  let mayMergeVisibleLeaf = !minimumVisibleDepthEnabled || !visible ||
      depth > minimumVisibleDepth;
  if ((belowVisibleFloor ||
       error > settings.lodThresholds.x * settings.lodThresholds.y) &&
      depth < maximumDepth &&
      counters.freeCount > settings.minimumFreeSlots) {
    command |= PLANET_CBT_COMMAND_SPLIT;
  } else if (error < settings.lodThresholds.x * settings.lodThresholds.z &&
      depth > 0u && mayMergeVisibleLeaf) {
    command |= PLANET_CBT_COMMAND_MERGE;
  }
  return command;
}

fn splitRequest(slot: u32) {
  if (!validSlot(slot) || bisectors[slot].depth >= settings.maximumDepth) {
    return;
  }
  var stack: array<u32, PLANET_CBT_MAXIMUM_COMPATIBILITY_CHAIN>;
  var stackCount = 0u;
  var current = slot;
  var valid = true;
  loop {
    if (bisectors[current].depth >= settings.maximumDepth) {
      return;
    }
    let neighbor = bisectors[current].twin;
    if (!validSlot(neighbor)) {
      valid = false;
      break;
    }
    if (bisectors[neighbor].depth >= settings.maximumDepth) {
      return;
    }
    if (bisectors[neighbor].twin == current) {
      break;
    }
    if (stackCount >= PLANET_CBT_MAXIMUM_COMPATIBILITY_CHAIN) {
      valid = false;
      break;
    }
    stack[stackCount] = current;
    stackCount += 1u;
    current = neighbor;
  }
  let pairCount = stackCount + 1u;
  let required = pairCount * 2u;
  if (!valid || counters.requestedAllocations + required > counters.freeCount) {
    counters.overflowCount += 1u;
    atomicOr(&bisectors[slot].command,
        PLANET_CBT_COMMAND_REJECTED_CAPACITY);
    return;
  }
  if (counters.requestedAllocations + required >
      settings.maximumAllocationsPerRound) {
    return;
  }

  var cursor = counters.requestedAllocations;
  var neighbor = bisectors[current].twin;
  splitCompatiblePair(current, neighbor,
      pointerCache[cursor].freePoolSlot,
      pointerCache[cursor + 1u].freePoolSlot);
  cursor += 2u;
  while (stackCount > 0u) {
    stackCount -= 1u;
    let pending = stack[stackCount];
    neighbor = bisectors[pending].twin;
    if (!validSlot(pending) || !validSlot(neighbor) ||
        bisectors[neighbor].twin != pending) {
      counters.overflowCount += 1u;
      return;
    }
    splitCompatiblePair(pending, neighbor,
        pointerCache[cursor].freePoolSlot,
        pointerCache[cursor + 1u].freePoolSlot);
    cursor += 2u;
  }
  counters.requestedAllocations = cursor;
}

fn mergeRequest(slot: u32) {
  if (!validSlot(slot) || bisectors[slot].depth == 0u) {
    return;
  }
  var child0 = slot;
  var child1 = slot;
  if ((bisectors[slot].pathLow & 1u) == 0u) {
    child1 = bisectors[slot].next;
  } else {
    child0 = bisectors[slot].previous;
  }
  if (!addressesAreSiblings(child0, child1)) {
    return;
  }
  let secondChild1 = bisectors[child0].previous;
  let secondChild0 = bisectors[child1].next;
  if (!addressesAreSiblings(secondChild0, secondChild1)) {
    return;
  }
  let slots = array<u32, 4>(child0, child1, secondChild0, secondChild1);
  for (var index = 0u; index < 4u; index += 1u) {
    let command = atomicLoad(&bisectors[slots[index]].command);
    if ((command & PLANET_CBT_COMMAND_SPLIT) != 0u ||
        (command & PLANET_CBT_COMMAND_MERGE) == 0u) {
      return;
    }
  }

  let firstPrevious = bisectors[child0].twin;
  let firstNext = bisectors[child1].twin;
  let secondPrevious = bisectors[secondChild0].twin;
  let secondNext = bisectors[secondChild1].twin;
  let firstRoot = bisectors[child0].rootHalfedge;
  let firstDepth = bisectors[child0].depth - 1u;
  let firstLow = parentPathLow(
      bisectors[child0].pathLow, bisectors[child0].pathHigh);
  let firstHigh = parentPathHigh(bisectors[child0].pathHigh);
  let secondRoot = bisectors[secondChild0].rootHalfedge;
  let secondDepth = bisectors[secondChild0].depth - 1u;
  let secondLow = parentPathLow(
      bisectors[secondChild0].pathLow, bisectors[secondChild0].pathHigh);
  let secondHigh = parentPathHigh(bisectors[secondChild0].pathHigh);

  replaceNeighbor(firstNext, child1, child0);
  replaceNeighbor(secondNext, secondChild1, secondChild0);
  writeBisector(child0, firstNext, firstPrevious, secondChild0,
      firstRoot, firstDepth, firstLow, firstHigh);
  writeBisector(secondChild0, secondNext, secondPrevious, child0,
      secondRoot, secondDepth, secondLow, secondHigh);

  bisectors[child1].next = PLANET_CBT_NULL_SLOT;
  bisectors[child1].previous = PLANET_CBT_NULL_SLOT;
  bisectors[child1].twin = PLANET_CBT_NULL_SLOT;
  atomicStore(&bisectors[child1].command, PLANET_CBT_COMMAND_ACCEPTED);
  bisectors[child1].rootHalfedge = PLANET_CBT_NULL_SLOT;
  bisectors[secondChild1].next = PLANET_CBT_NULL_SLOT;
  bisectors[secondChild1].previous = PLANET_CBT_NULL_SLOT;
  bisectors[secondChild1].twin = PLANET_CBT_NULL_SLOT;
  atomicStore(&bisectors[secondChild1].command, PLANET_CBT_COMMAND_ACCEPTED);
  bisectors[secondChild1].rootHalfedge = PLANET_CBT_NULL_SLOT;
  bitfieldChanges[child1] = PLANET_CBT_BITFIELD_RELEASE;
  bitfieldChanges[secondChild1] = PLANET_CBT_BITFIELD_RELEASE;
  setHeapLeaf(child1, false);
  setHeapLeaf(secondChild1, false);
  counters.mergeCount += 2u;
}

@compute @workgroup_size(256, 1, 1)
fn resetPlanetCbtCommands(
    @builtin(global_invocation_id) globalId: vec3<u32>) {
  if (!planetCbtRoundEnabled()) {
    return;
  }
  let activeIndex = globalId.x;
  if (activeIndex >= counters.activeCount) {
    return;
  }
  let slot = pointerCache[activeIndex].activePoolSlot;
  if (!validSlot(slot)) {
    return;
  }
  atomicStore(&bisectors[slot].command, PLANET_CBT_COMMAND_NONE);
  for (var index = 0u; index < 4u; index += 1u) {
    bisectors[slot].allocation[index] = PLANET_CBT_NULL_SLOT;
  }
  bitfieldChanges[slot] = PLANET_CBT_BITFIELD_UNCHANGED;
}

@compute @workgroup_size(256, 1, 1)
fn generatePlanetCbtCommands(
    @builtin(global_invocation_id) globalId: vec3<u32>) {
  if (!planetCbtRoundEnabled()) {
    return;
  }
  let activeIndex = globalId.x;
  if (activeIndex >= counters.activeCount) {
    return;
  }
  let slot = pointerCache[activeIndex].activePoolSlot;
  if (!validSlot(slot)) {
    return;
  }
  let command = generatedCommand(slot);
  if ((command & (PLANET_CBT_COMMAND_SPLIT |
      PLANET_CBT_COMMAND_MERGE)) != 0u) {
    let commandIndex = atomicAdd(&counters.unchangedCount, 1u);
    if (commandIndex < arrayLength(&commandIndices)) {
      commandIndices[commandIndex] = slot;
    }
  }
  if ((command & PLANET_CBT_COMMAND_SPLIT) != 0u) {
    markSplitCompatibilityChain(slot);
  }
  if ((command & PLANET_CBT_COMMAND_MERGE) != 0u) {
    markMergeDiamond(slot);
  }
}

@compute @workgroup_size(1, 1, 1)
fn reservePlanetCbtBlocks(
    @builtin(global_invocation_id) globalId: vec3<u32>) {
  if (!planetCbtRoundEnabled() || any(globalId != vec3<u32>(0u))) {
    return;
  }
  let initialActiveCount = counters.activeCount;
  let commandCount = min(atomicLoad(&counters.unchangedCount),
      arrayLength(&commandIndices));
  for (var commandIndex = 0u; commandIndex < commandCount;
       commandIndex += 1u) {
    let slot = commandIndices[commandIndex];
    if (!validSlot(slot)) {
      continue;
    }
    let command = atomicLoad(&bisectors[slot].command);
    if ((command & PLANET_CBT_COMMAND_SPLIT) != 0u) {
      splitRequest(slot);
    }
  }
  for (var commandIndex = 0u; commandIndex < commandCount;
       commandIndex += 1u) {
    let slot = commandIndices[commandIndex];
    if (!validSlot(slot)) {
      continue;
    }
    let command = atomicLoad(&bisectors[slot].command);
    if ((command & PLANET_CBT_COMMAND_SPLIT) == 0u &&
        (command & PLANET_CBT_COMMAND_MERGE) != 0u) {
      mergeRequest(slot);
    }
  }
  atomicStore(&counters.unchangedCount,
      initialActiveCount - min(initialActiveCount, commandCount));
}

@compute @workgroup_size(256, 1, 1)
fn fillPlanetCbtBlocks(
    @builtin(global_invocation_id) globalId: vec3<u32>) {
  if (!planetCbtRoundEnabled()) {
    return;
  }
  let slot = globalId.x;
  if (slot >= arrayLength(&bisectors) ||
      bitfieldChanges[slot] != PLANET_CBT_BITFIELD_ACTIVATE) {
    return;
  }
  atomicAnd(&bisectors[slot].command,
      PLANET_CBT_COMMAND_ACCEPTED);
}

@compute @workgroup_size(1, 1, 1)
fn repairPlanetCbtNeighbors(
    @builtin(global_invocation_id) globalId: vec3<u32>) {
  if (!planetCbtRoundEnabled() || any(globalId != vec3<u32>(0u))) {
    return;
  }
  var maximumDepth = 0u;
  for (var activeIndex = 0u; activeIndex < counters.activeCount;
       activeIndex += 1u) {
    let slot = pointerCache[activeIndex].activePoolSlot;
    if (!validSlot(slot)) {
      counters.overflowCount += 1u;
      continue;
    }
    let change = bitfieldChanges[slot];
    let isActive = select(cbtHeap[arrayLength(&bisectors) + slot] != 0u,
        true, change == PLANET_CBT_BITFIELD_ACTIVATE);
    if (!isActive || change == PLANET_CBT_BITFIELD_RELEASE) {
      continue;
    }
    maximumDepth = max(maximumDepth, bisectors[slot].depth);
    let neighbors = array<u32, 3>(bisectors[slot].next,
        bisectors[slot].previous, bisectors[slot].twin);
    for (var edge = 0u; edge < 3u; edge += 1u) {
      let neighbor = neighbors[edge];
      if (!validSlot(neighbor) ||
          (bisectors[neighbor].next != slot &&
           bisectors[neighbor].previous != slot &&
           bisectors[neighbor].twin != slot)) {
        counters.overflowCount += 1u;
      }
    }
  }
  counters.maximumDepth = maximumDepth;
}

@compute @workgroup_size(256, 1, 1)
fn updatePlanetCbtBitfield(
    @builtin(global_invocation_id) globalId: vec3<u32>) {
  if (!planetCbtRoundEnabled()) {
    return;
  }
  let slot = globalId.x;
  if (slot >= arrayLength(&bisectors)) {
    return;
  }
  let change = bitfieldChanges[slot];
  if (change == PLANET_CBT_BITFIELD_ACTIVATE) {
    cbtHeap[arrayLength(&bisectors) + slot] = 1u;
  } else if (change == PLANET_CBT_BITFIELD_RELEASE) {
    cbtHeap[arrayLength(&bisectors) + slot] = 0u;
  }
}
