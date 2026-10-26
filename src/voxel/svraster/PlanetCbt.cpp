#include "voxel/svraster/PlanetCbt.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace voxel::svraster
{

namespace
{

bool planetCbtLifecycleFloatChanged(const float first, const float second)
{
  const float scale = std::max({1.0f, std::abs(first), std::abs(second)});
  return !std::isfinite(first) || !std::isfinite(second) ||
      std::abs(first - second) > scale * 1.0e-5f;
}

bool planetCbtLifecycleArrayChanged(
    const std::array<float, 4> &first,
    const std::array<float, 4> &second)
{
  for (size_t index = 0u; index < first.size(); ++index)
    if (planetCbtLifecycleFloatChanged(first[index], second[index]))
      return true;
  return false;
}

bool planetCbtLifecycleSettingsChanged(
    const PlanetCbtUpdateSettingsGPU &first,
    const PlanetCbtUpdateSettingsGPU &second)
{
  return planetCbtLifecycleArrayChanged(
             first.viewportFocal, second.viewportFocal) ||
      planetCbtLifecycleArrayChanged(
          first.cameraDirectionDistance, second.cameraDirectionDistance) ||
      planetCbtLifecycleArrayChanged(first.cameraRight, second.cameraRight) ||
      planetCbtLifecycleArrayChanged(first.cameraUp, second.cameraUp) ||
      planetCbtLifecycleArrayChanged(
          first.lodThresholds, second.lodThresholds) ||
      first.enableProjectedLod != second.enableProjectedLod ||
      first.maximumDepth != second.maximumDepth;
}

} // namespace

PlanetCbtLifecycleDecision PlanetCbtLifecycle::advance(
    const PlanetCbtUpdateSettingsGPU &settings,
    const PlanetCbtLifecycleInput &input)
{
  PlanetCbtLifecycleDecision result{};
  result.frameIndex = frameIndex_++;
  result.overviewVisible = input.adaptiveGeometry &&
      std::isfinite(input.overviewContribution) &&
      input.overviewContribution > 0.001f;

  const bool packageChanged = initialized_ &&
      input.packageGeneration != packageGeneration_;
  const bool becameVisible = result.overviewVisible && !wasVisible_;
  const bool settingsChanged = !haveSettings_ ||
      planetCbtLifecycleSettingsChanged(settings, previousSettings_);

  if (!initialized_)
  {
    initialized_ = true;
    packageGeneration_ = input.packageGeneration;
  }
  else if (packageChanged)
  {
    packageGeneration_ = input.packageGeneration;
    result.resetTopology = true;
    warmupFramesRemaining_ = kPlanetCbtWarmupFrameCount;
    convergenceFramesRemaining_ = kPlanetCbtConvergenceFrameCount;
  }

  if (becameVisible && input.updatesEnabled)
  {
    warmupFramesRemaining_ = kPlanetCbtWarmupFrameCount;
    convergenceFramesRemaining_ = kPlanetCbtConvergenceFrameCount;
  }
  else if (result.overviewVisible && input.updatesEnabled && settingsChanged &&
      !packageChanged)
  {
    // Continuous orbit manipulation receives one topology round per frame.
    // The larger burst is reserved for a reveal, avoiding transient spikes
    // during rapid drag and wheel input.
    convergenceFramesRemaining_ = kPlanetCbtConvergenceFrameCount;
  }

  if (result.overviewVisible && input.updatesEnabled && !packageChanged)
  {
    if (warmupFramesRemaining_ > 0u)
    {
      result.updateRoundCount = kPlanetCbtMaximumUpdateRounds;
      --warmupFramesRemaining_;
    }
    else if (convergenceFramesRemaining_ > 0u)
    {
      result.updateRoundCount = 1u;
      --convergenceFramesRemaining_;
    }
  }

  result.settled = result.updateRoundCount == 0u && !result.resetTopology;
  previousSettings_ = settings;
  haveSettings_ = true;
  wasVisible_ = result.overviewVisible;
  return result;
}

void PlanetCbtLifecycle::invalidate()
{
  initialized_ = false;
  wasVisible_ = false;
  haveSettings_ = false;
  packageGeneration_ = 0u;
  frameIndex_ = 0u;
  warmupFramesRemaining_ = 0u;
  convergenceFramesRemaining_ = 0u;
  previousSettings_ = {};
}

procedural::PlanetSurfaceBisectorAddress planetCbtBisectorAddress(const PlanetCbtBisectorGPU &bisector)
{
  return {
    bisector.rootHalfedge,
    bisector.depth,
    static_cast<uint64_t>(bisector.pathLow) | (static_cast<uint64_t>(bisector.pathHigh) << 32u),
  };
}

void setPlanetCbtBisectorAddress(PlanetCbtBisectorGPU &bisector, const procedural::PlanetSurfaceBisectorAddress &address)
{
  bisector.rootHalfedge = address.rootHalfedge;
  bisector.depth = address.depth;
  bisector.pathLow = static_cast<uint32_t>(address.path);
  bisector.pathHigh = static_cast<uint32_t>(address.path >> 32u);
}

bool buildPlanetCbtIcosahedron(procedural::PlanetSurfaceHalfedgeMesh &mesh)
{
  constexpr double phi = 1.6180339887498948482;
  std::vector<procedural::SpherePoint> vertices{
    {-1.0, phi, 0.0},
    {1.0, phi, 0.0},
    {-1.0, -phi, 0.0},
    {1.0, -phi, 0.0},
    {0.0, -1.0, phi},
    {0.0, 1.0, phi},
    {0.0, -1.0, -phi},
    {0.0, 1.0, -phi},
    {phi, 0.0, -1.0},
    {phi, 0.0, 1.0},
    {-phi, 0.0, -1.0},
    {-phi, 0.0, 1.0},
  };
  for (auto &vertex : vertices)
  {
    const double length = std::sqrt(vertex.x * vertex.x + vertex.y * vertex.y + vertex.z * vertex.z);
    vertex.x /= length;
    vertex.y /= length;
    vertex.z /= length;
  }
  std::vector<std::array<uint32_t, 3>> faces{
    {0u, 11u, 5u}, {0u, 5u, 1u}, {0u, 1u, 7u}, {0u, 7u, 10u}, {0u, 10u, 11u}, {1u, 5u, 9u}, {5u, 11u, 4u}, {11u, 10u, 2u}, {10u, 7u, 6u}, {7u, 1u, 8u},
    {3u, 9u, 4u},  {3u, 4u, 2u}, {3u, 2u, 6u}, {3u, 6u, 8u},  {3u, 8u, 9u},   {4u, 9u, 5u}, {2u, 4u, 11u}, {6u, 2u, 10u},  {8u, 6u, 7u},  {9u, 8u, 1u},
  };

  // Keep this constructor resilient to face-table edits by enforcing outward
  // winding from geometry rather than relying on a remembered convention.
  for (auto &face : faces)
  {
    const auto &a = vertices[face[0]];
    const auto &b = vertices[face[1]];
    const auto &c = vertices[face[2]];
    const double abx = b.x - a.x, aby = b.y - a.y, abz = b.z - a.z;
    const double acx = c.x - a.x, acy = c.y - a.y, acz = c.z - a.z;
    const double nx = aby * acz - abz * acy;
    const double ny = abz * acx - abx * acz;
    const double nz = abx * acy - aby * acx;
    const double cx = a.x + b.x + c.x;
    const double cy = a.y + b.y + c.y;
    const double cz = a.z + b.z + c.z;
    if (nx * cx + ny * cy + nz * cz < 0.0)
      std::swap(face[1], face[2]);
  }
  return procedural::buildPlanetSurfaceHalfedgeMesh(vertices, faces, mesh);
}

std::vector<PlanetCbtRootTriangleGPU> buildPlanetCbtRootTriangles(const procedural::PlanetSurfaceHalfedgeMesh &mesh)
{
  std::vector<PlanetCbtRootTriangleGPU> result;
  if (!procedural::validatePlanetSurfaceHalfedgeMesh(mesh))
    return result;
  result.resize(mesh.vertex.size());
  for (uint32_t root = 0u; root < mesh.vertex.size(); ++root)
  {
    const auto &a = mesh.vertices[mesh.vertex[root]];
    const auto &b = mesh.vertices[mesh.vertex[mesh.next[root]]];
    const auto &c = mesh.faceCentres[mesh.face[root]];
    result[root] = {
      {static_cast<float>(a.x), static_cast<float>(a.y), static_cast<float>(a.z), 0.0f},
      {static_cast<float>(b.x), static_cast<float>(b.y), static_cast<float>(b.z), 0.0f},
      {static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.z), 0.0f},
    };
  }
  return result;
}

uint32_t planetCbtPoolCapacity(const uint32_t treeDepth)
{
  return treeDepth < 31u ? uint32_t{1u} << treeDepth : 0u;
}

size_t planetCbtHeapEntryCount(const uint32_t treeDepth)
{
  return treeDepth < 31u ? size_t{1u} << (treeDepth + 1u) : 0u;
}

uint64_t planetCbtStorageBytes(const uint32_t treeDepth)
{
  const uint32_t capacity = planetCbtPoolCapacity(treeDepth);
  const size_t heapEntries = planetCbtHeapEntryCount(treeDepth);
  if (capacity < kPlanetCbtRootBisectorCount || heapEntries == 0u)
    return 0u;
  const uint64_t scheduledDispatchBytes =
      uint64_t{kPlanetCbtMaximumUpdateRounds} * (treeDepth + 2u) *
      sizeof(PlanetCbtDispatchIndirectGPU);
  return uint64_t{capacity} * sizeof(PlanetCbtBisectorGPU) +
      uint64_t{heapEntries} * sizeof(uint32_t) +
      uint64_t{capacity} * sizeof(PlanetCbtPointerGPU) +
      sizeof(PlanetCbtCountersGPU) + sizeof(PlanetCbtDispatchesGPU) +
      sizeof(PlanetCbtDrawIndirectGPU) +
      uint64_t{kPlanetCbtRootBisectorCount} *
          sizeof(PlanetCbtRootTriangleGPU) +
      uint64_t{kPlanetCbtMaximumForcedRequests} *
          sizeof(PlanetCbtRequestGPU) +
      uint64_t{capacity} * sizeof(uint32_t) * 2u +
      sizeof(PlanetCbtUpdateSettingsGPU) + scheduledDispatchBytes;
}

bool planetCbtAdaptiveSupported(
    const PlanetCbtDeviceCapabilities &capabilities,
    const uint32_t treeDepth)
{
  const uint64_t storageBytes = planetCbtStorageBytes(treeDepth);
  return capabilities.compute && capabilities.graphics &&
      capabilities.maximumComputeWorkgroupInvocations >=
          kPlanetCbtMinimumWorkgroupInvocations &&
      storageBytes != 0u && storageBytes <= kPlanetCbtMaximumStorageBytes &&
      capabilities.memoryBytes >= storageBytes;
}

bool planetCbtSetLeafBit(std::vector<uint32_t> &heap, const uint32_t treeDepth, const uint32_t poolSlot, const bool allocated)
{
  const uint32_t capacity = planetCbtPoolCapacity(treeDepth);
  if (capacity == 0u || poolSlot >= capacity || heap.size() != planetCbtHeapEntryCount(treeDepth))
    return false;
  heap[capacity + poolSlot] = allocated ? 1u : 0u;
  return true;
}

bool planetCbtReduce(std::vector<uint32_t> &heap, const uint32_t treeDepth)
{
  const uint32_t capacity = planetCbtPoolCapacity(treeDepth);
  if (capacity == 0u || heap.size() != planetCbtHeapEntryCount(treeDepth))
    return false;
  for (uint32_t node = capacity - 1u; node > 0u; --node)
  {
    const uint64_t sum = static_cast<uint64_t>(heap[node * 2u]) + heap[node * 2u + 1u];
    if (sum > std::numeric_limits<uint32_t>::max())
      return false;
    heap[node] = static_cast<uint32_t>(sum);
  }
  heap[0] = 0u;
  return true;
}

uint32_t planetCbtOneToBitID(const std::vector<uint32_t> &heap, const uint32_t treeDepth, uint32_t allocatedIndex)
{
  const uint32_t capacity = planetCbtPoolCapacity(treeDepth);
  if (capacity == 0u || heap.size() != planetCbtHeapEntryCount(treeDepth) || allocatedIndex >= heap[1u])
    return kPlanetCbtNullSlot;
  uint32_t node = 1u;
  for (uint32_t level = 0u; level < treeDepth; ++level)
  {
    const uint32_t left = node * 2u;
    if (allocatedIndex < heap[left])
      node = left;
    else
    {
      allocatedIndex -= heap[left];
      node = left + 1u;
    }
  }
  return node - capacity;
}

uint32_t planetCbtZeroToBitID(const std::vector<uint32_t> &heap, const uint32_t treeDepth, uint32_t freeIndex)
{
  const uint32_t capacity = planetCbtPoolCapacity(treeDepth);
  if (capacity == 0u || heap.size() != planetCbtHeapEntryCount(treeDepth) || freeIndex >= capacity - heap[1u])
    return kPlanetCbtNullSlot;
  uint32_t node = 1u;
  uint32_t subtreeCapacity = capacity;
  for (uint32_t level = 0u; level < treeDepth; ++level)
  {
    const uint32_t left = node * 2u;
    const uint32_t halfCapacity = subtreeCapacity / 2u;
    const uint32_t leftFree = halfCapacity - heap[left];
    if (freeIndex < leftFree)
      node = left;
    else
    {
      freeIndex -= leftFree;
      node = left + 1u;
    }
    subtreeCapacity = halfCapacity;
  }
  return node - capacity;
}

} // namespace voxel::svraster
