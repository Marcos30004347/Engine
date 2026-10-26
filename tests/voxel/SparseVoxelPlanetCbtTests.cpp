#include "voxel/svraster/PlanetCbtReference.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace
{

int failures = 0;

void check(const bool condition, const char *message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

bool samePoint(const procedural::SpherePoint &a, const procedural::SpherePoint &b)
{
  return std::abs(a.x - b.x) <= 1.0e-13 && std::abs(a.y - b.y) <= 1.0e-13 && std::abs(a.z - b.z) <= 1.0e-13;
}

bool sameAddresses(const std::vector<procedural::PlanetSurfaceBisectorAddress> &a, const std::vector<procedural::PlanetSurfaceBisectorAddress> &b)
{
  if (a.size() != b.size())
    return false;
  for (size_t index = 0u; index < a.size(); ++index)
    if (a[index].rootHalfedge != b[index].rootHalfedge || a[index].depth != b[index].depth || a[index].path != b[index].path)
      return false;
  return true;
}

void testIcosahedronAndDecoding()
{
  std::cout << "planet CBT icosahedron and stable decoding\n";
  procedural::PlanetSurfaceHalfedgeMesh mesh;
  check(voxel::svraster::buildPlanetCbtIcosahedron(mesh), "normalized icosahedron converts to a halfedge mesh");
  check(mesh.vertices.size() == 12u && mesh.faceCentres.size() == 20u && mesh.vertex.size() == voxel::svraster::kPlanetCbtRootBisectorCount, "20 faces produce exactly 60 root bisectors");
  bool closed = true;
  bool reciprocal = true;
  bool normalized = true;
  bool outward = true;
  for (uint32_t halfedge = 0u; halfedge < mesh.vertex.size(); ++halfedge)
  {
    closed &= mesh.twin[halfedge] != procedural::PlanetSurfaceHalfedgeMesh::noHalfedge;
    reciprocal &= mesh.twin[mesh.twin[halfedge]] == halfedge;
  }
  for (const auto &vertex : mesh.vertices)
    normalized &= std::abs(vertex.x * vertex.x + vertex.y * vertex.y + vertex.z * vertex.z - 1.0) < 1.0e-13;
  for (uint32_t face = 0u; face < mesh.faceCentres.size(); ++face)
  {
    const uint32_t base = face * 3u;
    const auto &a = mesh.vertices[mesh.vertex[base]];
    const auto &b = mesh.vertices[mesh.vertex[base + 1u]];
    const auto &c = mesh.vertices[mesh.vertex[base + 2u]];
    const double nx = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y);
    const double ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
    const double nz = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    outward &= nx * (a.x + b.x + c.x) + ny * (a.y + b.y + c.y) + nz * (a.z + b.z + c.z) > 0.0;
  }
  check(closed && reciprocal, "all coarse edges have reciprocal twins");
  check(normalized && outward, "coarse vertices are unit length and consistently outward wound");

  // Every face is partitioned by its three edge-to-centre roots. Their base
  // edges are the face boundary and each centre leg occurs twice.
  bool rootsCoverFaces = true;
  for (uint32_t face = 0u; face < 20u; ++face)
  {
    const uint32_t base = face * 3u;
    rootsCoverFaces &= mesh.next[base] == base + 1u && mesh.next[base + 1u] == base + 2u && mesh.next[base + 2u] == base;
    for (uint32_t edge = 0u; edge < 3u; ++edge)
    {
      procedural::PlanetSurfaceTriangle triangle{};
      rootsCoverFaces &= procedural::planetSurfaceBisectorTriangle(mesh, {base + edge, 0u, 0u}, triangle);
      const auto centre = mesh.faceCentres[face];
      const double length = std::sqrt(centre.x * centre.x + centre.y * centre.y + centre.z * centre.z);
      rootsCoverFaces &= samePoint(triangle.vertices[2], {centre.x / length, centre.y / length, centre.z / length});
    }
  }
  check(rootsCoverFaces, "three non-overlapping bisector roots cover every coarse face");

  std::mt19937_64 random(0x8bd96a4f21c37e5bull);
  bool decodersMatch = true;
  for (uint32_t sample = 0u; sample < 10000u; ++sample)
  {
    const uint32_t depth = static_cast<uint32_t>(random() % 25u);
    const uint64_t path = depth == 0u ? 0u : random() & ((uint64_t{1u} << depth) - 1u);
    const procedural::PlanetSurfaceBisectorAddress address{static_cast<uint32_t>(random() % 60u), depth, path};
    procedural::PlanetSurfaceTriangle existing{}, reference{};
    decodersMatch &= procedural::planetSurfaceBisectorTriangle(mesh, address, existing) && voxel::svraster::planetCbtReferenceTriangle(mesh, address, reference);
    for (uint32_t corner = 0u; corner < 3u; ++corner)
      decodersMatch &= samePoint(existing.vertices[corner], reference.vertices[corner]);
  }
  check(decodersMatch, "10,000 seeded addresses through depth 24 match the existing decoder");
}

void testAllocator()
{
  std::cout << "planet CBT allocator reference\n";
  voxel::svraster::PlanetCbtBisectorGPU gpuAddress{};
  const procedural::PlanetSurfaceBisectorAddress deepest{59u, 63u, (uint64_t{1u} << 63u) - 1u};
  voxel::svraster::setPlanetCbtBisectorAddress(gpuAddress, deepest);
  const auto decoded = voxel::svraster::planetCbtBisectorAddress(gpuAddress);
  check(
      decoded.rootHalfedge == deepest.rootHalfedge && decoded.depth == deepest.depth && decoded.path == deepest.path && gpuAddress.pathLow == 0xffffffffu && gpuAddress.pathHigh == 0x7fffffffu,
      "two-word GPU paths preserve all 63 supported address bits");

  constexpr uint32_t depth = 8u;
  constexpr uint32_t capacity = 1u << depth;
  std::vector<uint32_t> heap(voxel::svraster::planetCbtHeapEntryCount(depth), 0u);
  std::mt19937 random(0x41c6ce57u);
  std::vector<uint32_t> active, free;
  for (uint32_t slot = 0u; slot < capacity; ++slot)
  {
    const bool allocated = (random() & 3u) != 0u;
    voxel::svraster::planetCbtSetLeafBit(heap, depth, slot, allocated);
    (allocated ? active : free).push_back(slot);
  }
  check(voxel::svraster::planetCbtReduce(heap, depth) && heap[1u] == active.size(), "bottom-up reduction produces the active root count");
  bool enumeration = true;
  for (uint32_t index = 0u; index < active.size(); ++index)
    enumeration &= voxel::svraster::planetCbtOneToBitID(heap, depth, index) == active[index];
  for (uint32_t index = 0u; index < free.size(); ++index)
    enumeration &= voxel::svraster::planetCbtZeroToBitID(heap, depth, index) == free[index];
  enumeration &= voxel::svraster::planetCbtOneToBitID(heap, depth, active.size()) == voxel::svraster::kPlanetCbtNullSlot;
  enumeration &= voxel::svraster::planetCbtZeroToBitID(heap, depth, free.size()) == voxel::svraster::kPlanetCbtNullSlot;
  check(enumeration, "one/zero lookup enumerates each leaf in slot order");

  bool boundaryPatterns = true;
  for (const uint32_t pattern : {0u, 1u, 2u})
  {
    std::vector<uint32_t> patternHeap(voxel::svraster::planetCbtHeapEntryCount(6u), 0u);
    for (uint32_t slot = 0u; slot < 64u; ++slot)
      boundaryPatterns &= voxel::svraster::planetCbtSetLeafBit(patternHeap, 6u, slot, pattern == 1u || (pattern == 2u && (slot & 1u) == 0u));
    boundaryPatterns &= voxel::svraster::planetCbtReduce(patternHeap, 6u) && patternHeap[1u] == (pattern == 0u ? 0u : pattern == 1u ? 64u : 32u);
  }
  check(boundaryPatterns, "empty, full, and alternating leaf patterns reduce correctly");
}

void testTopologyTransactionsAndStress()
{
  std::cout << "planet CBT conforming topology\n";
  procedural::PlanetSurfaceHalfedgeMesh mesh;
  voxel::svraster::buildPlanetCbtIcosahedron(mesh);
  voxel::svraster::PlanetCbtReference reference;
  check(reference.initialize(mesh, 8u) && reference.activeCount() == 60u && reference.validate(), "root initialization is closed and topology-valid");
  const auto roots = reference.activeAddresses();
  check(reference.split(roots.front()) && reference.activeCount() == 62u && reference.validate(), "a root split updates its compatible twin as one conforming diamond");
  const auto firstChildren = reference.activeAddresses();
  const auto child = std::find_if(
      firstChildren.begin(),
      firstChildren.end(),
      [&roots](const auto &address)
      {
        return address.rootHalfedge == roots.front().rootHalfedge && address.depth == 1u;
      });
  check(child != firstChildren.end() && reference.split(*child) && reference.validate(), "a deeper request propagates and repairs its compatibility chain");

  // Greedily merge deepest leaves. Conservative configurations that are not
  // ready simply reject; reverse-order diamonds eventually restore all roots.
  uint32_t mergeAttempts = 0u;
  while (reference.activeCount() > 60u && mergeAttempts++ < 10000u)
  {
    auto leaves = reference.activeAddresses();
    std::sort(
        leaves.begin(),
        leaves.end(),
        [](const auto &a, const auto &b)
        {
          return a.depth > b.depth;
        });
    bool merged = false;
    for (const auto &leaf : leaves)
      if (reference.merge(leaf))
      {
        merged = true;
        break;
      }
    if (!merged)
      break;
  }
  check(reference.activeCount() == 60u && sameAddresses(reference.activeAddresses(), roots) && reference.validate(), "conservative reverse merges recover the identical stable-root multiset");

  voxel::svraster::PlanetCbtReference bounded;
  check(bounded.initialize(mesh, 6u), "minimum practical 64-slot pool initializes");
  for (uint32_t root = 0u; root < 60u && bounded.activeCount() < bounded.capacity(); ++root)
  {
    const auto leaves = bounded.activeAddresses();
    const auto leaf = std::find_if(
        leaves.begin(),
        leaves.end(),
        [root](const auto &address)
        {
          return address.rootHalfedge == root && address.depth == 0u;
        });
    if (leaf != leaves.end())
      bounded.split(*leaf);
  }
  const auto beforePool = bounded.pool();
  const auto beforeHeap = bounded.heap();
  const auto beforeAddresses = bounded.activeAddresses();
  bool rejected = false;
  for (const auto &leaf : beforeAddresses)
    if (!bounded.split(leaf))
    {
      rejected = true;
      break;
    }
  check(
      rejected && beforeHeap == bounded.heap() && beforePool.size() == bounded.pool().size() && std::memcmp(beforePool.data(), bounded.pool().data(), beforePool.size() * sizeof(beforePool[0])) == 0 &&
          sameAddresses(beforeAddresses, bounded.activeAddresses()),
      "capacity rejection leaves pool, heap, and stable topology byte-for-byte unchanged");

  voxel::svraster::PlanetCbtReference stress;
  stress.initialize(mesh, 8u);
  std::mt19937_64 random(0x1d923ad4f0bc8675ull);
  bool valid = true;
  for (uint32_t request = 0u; request < 100000u && valid; ++request)
  {
    const auto leaves = stress.activeAddresses();
    const auto selected = leaves[static_cast<size_t>(random() % leaves.size())];
    if ((random() & 1u) == 0u && selected.depth < 8u)
      stress.split(selected);
    else
      stress.merge(selected);
    if ((request & 255u) == 0u)
      valid &= stress.validate();
  }
  valid &= stress.validate();
  check(valid, "100,000 seeded mixed split/merge requests preserve every invariant");
}

void testNavigationLifecyclePolicy()
{
  namespace svr = voxel::svraster;
  std::cout << "planet CBT navigation lifecycle\n";
  svr::PlanetCbtLifecycle lifecycle;
  svr::PlanetCbtUpdateSettingsGPU settings{};
  settings.enableProjectedLod = 1u;
  const svr::PlanetCbtLifecycleInput visible{
    .overviewContribution = 1.0f,
    .adaptiveGeometry = true,
    .packageGeneration = 7u,
  };
  bool warmupRounds = true;
  for (uint32_t frame = 0u; frame < svr::kPlanetCbtWarmupFrameCount;
       ++frame)
    warmupRounds &= lifecycle.advance(settings, visible).updateRoundCount ==
        svr::kPlanetCbtMaximumUpdateRounds;
  check(
      warmupRounds,
      "initial reveal schedules bounded multi-round warm-up frames");

  bool ordinaryRounds = true;
  for (uint32_t frame = 0u;
       frame < svr::kPlanetCbtConvergenceFrameCount; ++frame)
    ordinaryRounds &= lifecycle.advance(settings, visible).updateRoundCount == 1u;
  const auto settled = lifecycle.advance(settings, visible);
  check(
      ordinaryRounds && settled.updateRoundCount == 0u && settled.settled,
      "a stationary camera converges to zero scheduled topology churn");

  auto hidden = visible;
  hidden.overviewContribution = 0.0f;
  settings.cameraDirectionDistance[0] = 0.25f;
  const auto paused = lifecycle.advance(settings, hidden);
  check(
      paused.updateRoundCount == 0u && paused.settled,
      "zero overview contribution pauses adaptive updates");
  const auto revealedAgain = lifecycle.advance(settings, visible);
  check(
      revealedAgain.updateRoundCount == svr::kPlanetCbtMaximumUpdateRounds,
      "returning from the surface starts a bounded reveal warm-up");

  bool dragBounded = true;
  for (uint32_t frame = 0u; frame < 32u; ++frame)
  {
    settings.cameraDirectionDistance[0] += 0.01f;
    const auto decision = lifecycle.advance(settings, visible);
    dragBounded &= decision.updateRoundCount >= 1u &&
        decision.updateRoundCount <= svr::kPlanetCbtMaximumUpdateRounds;
  }
  check(dragBounded,
      "rapid orbit input keeps update rounds bounded without lifecycle reset");

  auto reloaded = visible;
  reloaded.packageGeneration = 8u;
  const auto reset = lifecycle.advance(settings, reloaded);
  const auto refineReloaded = lifecycle.advance(settings, reloaded);
  check(
      reset.resetTopology && reset.updateRoundCount == 0u &&
          refineReloaded.updateRoundCount ==
              svr::kPlanetCbtMaximumUpdateRounds,
      "package replacement exposes a root-only frame before refining");
}

void testPerformanceQualificationPolicy()
{
  namespace svr = voxel::svraster;
  std::cout << "planet CBT performance qualification policy\n";
  const uint64_t storage =
      svr::planetCbtStorageBytes(svr::kPlanetCbtDefaultDepth);
  check(storage > 0u && storage <= svr::kPlanetCbtMaximumStorageBytes,
      "default fixed CBT storage stays within the 10 MiB release gate");
  check(svr::planetCbtPoolCapacity(svr::kPlanetCbtDefaultDepth) >=
          svr::kPlanetCbtMaximumActiveTriangles,
      "default pool can hold the 128,000-triangle qualification ceiling");

  const svr::PlanetCbtDeviceCapabilities qualified{
    .compute = true,
    .graphics = true,
    .maximumComputeWorkgroupInvocations =
        svr::kPlanetCbtMinimumWorkgroupInvocations,
    .memoryBytes = storage,
  };
  check(svr::planetCbtAdaptiveSupported(qualified),
      "a device meeting the bounded compute, graphics, and memory contract enables adaptive CBT");
  auto unsupported = qualified;
  unsupported.compute = false;
  check(!svr::planetCbtAdaptiveSupported(unsupported),
      "missing compute support selects the static overview fallback");
  unsupported = qualified;
  unsupported.maximumComputeWorkgroupInvocations =
      svr::kPlanetCbtMinimumWorkgroupInvocations - 1u;
  check(!svr::planetCbtAdaptiveSupported(unsupported),
      "insufficient workgroup capacity selects the static overview fallback");
  unsupported = qualified;
  unsupported.memoryBytes = storage - 1u;
  check(!svr::planetCbtAdaptiveSupported(unsupported),
      "insufficient bounded storage selects the static overview fallback");
}

} // namespace

int main(const int argc, char **argv)
{
  if (argc > 1 && std::string(argv[1]) == "--phase6")
  {
    testNavigationLifecyclePolicy();
    return failures == 0 ? 0 : 1;
  }
  if (argc > 1 && std::string(argv[1]) == "--phase7")
  {
    testPerformanceQualificationPolicy();
    return failures == 0 ? 0 : 1;
  }
  testIcosahedronAndDecoding();
  testAllocator();
  testTopologyTransactionsAndStress();
  testNavigationLifecyclePolicy();
  testPerformanceQualificationPolicy();
  return failures == 0 ? 0 : 1;
}
