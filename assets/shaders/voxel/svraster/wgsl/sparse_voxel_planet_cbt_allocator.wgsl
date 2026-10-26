#include "sparse_voxel_planet_cbt_contract.wgsl"

struct PlanetCbtAllocatorSettings {
  treeDepth: u32,
  reductionLevel: u32,
  capacity: u32,
  workgroupSize: u32,
  roundIndex: u32,
  reserved0: u32,
  reserved1: u32,
  reserved2: u32,
}

@group(0) @binding(0) var<storage, read_write> cbtHeap: array<u32>;
@group(0) @binding(1) var<storage, read_write> pointerCache: array<PlanetCbtPointerGPU>;
@group(0) @binding(2) var<storage, read_write> counters: PlanetCbtCountersGPU;
@group(0) @binding(3) var<storage, read_write> dispatches: PlanetCbtDispatchesGPU;
@group(0) @binding(4) var<storage, read_write> drawIndirect: PlanetCbtDrawIndirectGPU;
@group(0) @binding(5) var<uniform> settings: PlanetCbtAllocatorSettings;
@group(0) @binding(6) var<uniform> updateSettings: PlanetCbtUpdateSettingsGPU;

fn planetCbtRoundEnabled() -> bool {
  return settings.roundIndex < updateSettings.updateRoundCount;
}

fn planetCbtDispatch(itemCount: u32) -> PlanetCbtDispatchIndirectGPU {
  return PlanetCbtDispatchIndirectGPU(
      (itemCount + settings.workgroupSize - 1u) / settings.workgroupSize,
      1u,
      1u,
      itemCount);
}

fn planetCbtOneToBitID(rank: u32) -> u32 {
  if (rank >= cbtHeap[1u]) {
    return PLANET_CBT_NULL_SLOT;
  }
  var remaining = rank;
  var node = 1u;
  for (var level = 0u; level < settings.treeDepth; level += 1u) {
    let left = node * 2u;
    let leftCount = cbtHeap[left];
    if (remaining < leftCount) {
      node = left;
    } else {
      remaining -= leftCount;
      node = left + 1u;
    }
  }
  return node - settings.capacity;
}

fn planetCbtZeroToBitID(rank: u32) -> u32 {
  let activeCount = min(cbtHeap[1u], settings.capacity);
  if (rank >= settings.capacity - activeCount) {
    return PLANET_CBT_NULL_SLOT;
  }
  var remaining = rank;
  var node = 1u;
  var subtreeCapacity = settings.capacity;
  for (var level = 0u; level < settings.treeDepth; level += 1u) {
    let left = node * 2u;
    let halfCapacity = subtreeCapacity / 2u;
    let leftFree = halfCapacity - min(cbtHeap[left], halfCapacity);
    if (remaining < leftFree) {
      node = left;
    } else {
      remaining -= leftFree;
      node = left + 1u;
    }
    subtreeCapacity = halfCapacity;
  }
  return node - settings.capacity;
}

@compute @workgroup_size(256, 1, 1)
fn reducePlanetCbtLevel(
    @builtin(global_invocation_id) globalId: vec3<u32>) {
  if (!planetCbtRoundEnabled() || settings.reductionLevel == 0u ||
      settings.reductionLevel > settings.treeDepth) {
    return;
  }
  let nodeCount = 1u << (settings.reductionLevel - 1u);
  let index = globalId.x;
  if (index >= nodeCount) {
    return;
  }
  let node = nodeCount + index;
  cbtHeap[node] = cbtHeap[node * 2u] + cbtHeap[node * 2u + 1u];
  if (node == 1u) {
    cbtHeap[0u] = 0u;
  }
}

@compute @workgroup_size(1, 1, 1)
fn resetPlanetCbtAllocator(
    @builtin(global_invocation_id) globalId: vec3<u32>) {
  // A paused lifecycle still runs the two one-thread round-zero resets. This
  // refreshes draw arguments and publishes zero split/merge diagnostics while
  // skipping pointer caching, topology mutation, and every reduction dispatch.
  if (any(globalId != vec3<u32>(0u))) {
    return;
  }
  let roundEnabled = planetCbtRoundEnabled();
  let pausedRefresh = updateSettings.updateRoundCount == 0u &&
      settings.roundIndex == 0u && settings.reductionLevel == 0u;
  if (!roundEnabled) {
    // The next stages consume these arguments indirectly. Zeroing them in the
    // first pass of every inactive round avoids launching capacity-sized
    // no-op grids merely to discover the round is disabled in the shader.
    if (settings.reductionLevel == 0u) {
      dispatches.activeDispatch = PlanetCbtDispatchIndirectGPU(0u, 1u, 1u, 0u);
      dispatches.freeDispatch = PlanetCbtDispatchIndirectGPU(0u, 1u, 1u, 0u);
      dispatches.pointerCacheDispatch =
          PlanetCbtDispatchIndirectGPU(0u, 1u, 1u, 0u);
    }
    if (!pausedRefresh) {
      return;
    }
  }
  let activeCount = min(cbtHeap[1u], settings.capacity);
  let freeCount = settings.capacity - activeCount;
  counters.activeCount = activeCount;
  counters.freeCount = freeCount;
  counters.requestedAllocations = 0u;
  // A zero reduction level begins a frame and clears diagnostics. A non-zero
  // level is the post-mutation refresh and deliberately preserves the result
  // of the topology transaction for consumers and validation readback.
  if (settings.reductionLevel == 0u) {
    counters.overflowCount = 0u;
    counters.splitCount = 0u;
    counters.mergeCount = 0u;
    atomicStore(&counters.unchangedCount, 0u);
    counters.maximumDepth = 0u;
    if (pausedRefresh) {
      // A settled lifecycle has no pending LOD action. Publish the stable
      // population so qualification can verify the no-churn/error-band gate
      // without a CPU topology scan.
      atomicStore(&counters.unchangedCount, activeCount);
    }
  }

  if (roundEnabled) {
    dispatches.activeDispatch = planetCbtDispatch(activeCount);
    dispatches.freeDispatch = planetCbtDispatch(freeCount);
    dispatches.pointerCacheDispatch =
        planetCbtDispatch(max(activeCount, min(freeCount,
            updateSettings.maximumAllocationsPerRound)));
  }
  let adaptiveDrawEnabled = (updateSettings.lifecycleFlags &
      PLANET_CBT_LIFECYCLE_DISABLE_ADAPTIVE_DRAW) == 0u;
  let renderSubdivisionDepth = min(
      updateSettings.renderSubdivisionDepth,
      PLANET_CBT_MAXIMUM_RENDER_SUBDIVISION_DEPTH);
  drawIndirect.vertexCount = select(0u, activeCount * 3u,
      adaptiveDrawEnabled);
  drawIndirect.instanceCount = select(1u,
      1u << renderSubdivisionDepth, adaptiveDrawEnabled);
  drawIndirect.firstVertex = 0u;
  drawIndirect.firstInstance = 0u;
}

@compute @workgroup_size(256, 1, 1)
fn cachePlanetCbtPointers(
    @builtin(global_invocation_id) globalId: vec3<u32>) {
  if (!planetCbtRoundEnabled()) {
    return;
  }
  let index = globalId.x;
  let workCount = max(counters.activeCount, counters.freeCount);
  if (index >= workCount) {
    return;
  }
  var pointers = PlanetCbtPointerGPU(
      PLANET_CBT_NULL_SLOT, PLANET_CBT_NULL_SLOT);
  if (index < counters.activeCount) {
    pointers.activePoolSlot = planetCbtOneToBitID(index);
  }
  if (index < counters.freeCount) {
    pointers.freePoolSlot = planetCbtZeroToBitID(index);
  }
  pointerCache[index] = pointers;
}
