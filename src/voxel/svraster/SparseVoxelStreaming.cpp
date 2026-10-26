#include "voxel/svraster/SparseVoxelStreaming.hpp"

#include "virtualgeometry/VirtualGeometryData.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>

namespace voxel::svraster
{
namespace
{

constexpr uint32_t kVisiblePriorityBit = 0x80000000u;
constexpr uint32_t kProjectedPriorityMask = 0x7ffffu;
constexpr uint32_t kDistancePriorityMask = 0xfffu;

uint32_t quantizeFinite(const float value, const float scale, const uint32_t maximum)
{
  if (!std::isfinite(value) || value <= 0.0f)
    return 0u;
  const double scaled = std::floor(static_cast<double>(value) * static_cast<double>(scale) + 0.5);
  return static_cast<uint32_t>(std::min<double>(scaled, maximum));
}

} // namespace

SparseVoxelMissingRegionCategory classifySparseVoxelMissingRegion(const SparseVoxelCutDiagnostics &diagnostics)
{
  if (diagnostics.selectedNeitherParentNorChildCount != 0u || diagnostics.invalidGenerationFallbackCount != 0u)
    return SparseVoxelMissingRegionCategory::CutSelection;
  if (diagnostics.hzbRejectionCount != 0u || diagnostics.frustumRejectionCount != 0u)
    return SparseVoxelMissingRegionCategory::HZBOrFrustum;
  if (diagnostics.rasterProxyCoverageMissCount != 0u)
    return SparseVoxelMissingRegionCategory::RasterProxyCoverage;
  if (diagnostics.tree64LightingMissCount != 0u)
    return SparseVoxelMissingRegionCategory::Tree64Lighting;
  if (diagnostics.pageResidencyMissCount != 0u)
    return SparseVoxelMissingRegionCategory::PageResidency;
  return SparseVoxelMissingRegionCategory::None;
}

SparseVoxelCutDiagnostics sparseVoxelCutDiagnosticsFromFrameCounters(const SparseVoxelFrameCountersGPU &counters)
{
  SparseVoxelCutDiagnostics diagnostics{};
  diagnostics.hzbRejectionCount = counters.occlusionCulledVoxelCount;
  return diagnostics;
}

uint32_t quantizeSparseVoxelPagePriority(const bool visible, const float projectedErrorPixels, const float distance)
{
  const uint32_t projected = quantizeFinite(projectedErrorPixels, 16.0f, kProjectedPriorityMask);
  const uint32_t quantizedDistance = !std::isfinite(distance) ? kDistancePriorityMask : quantizeFinite(std::max(distance, 0.0f), 16.0f, kDistancePriorityMask);
  const uint32_t inverseDistance = kDistancePriorityMask - quantizedDistance;
  return (visible ? kVisiblePriorityBit : 0u) | (projected << 12u) | inverseDistance;
}

std::vector<SparseVoxelPageTableDirtyRange> coalesceSparseVoxelPageTableDirtyRanges(
    const std::vector<uint32_t> &indices,
    const uint32_t maximumGap)
{
  if (indices.empty())
    return {};
  std::vector<uint32_t> sorted = indices;
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());

  std::vector<SparseVoxelPageTableDirtyRange> ranges;
  ranges.reserve(sorted.size());
  uint32_t first = sorted.front();
  uint32_t last = first;
  for (size_t i = 1u; i < sorted.size(); ++i)
  {
    const uint32_t index = sorted[i];
    const uint64_t gap = static_cast<uint64_t>(index) - last;
    if (index <= last || gap <= static_cast<uint64_t>(maximumGap) + 1ull)
    {
      last = index;
      continue;
    }
    ranges.push_back({first, last - first + 1u});
    first = last = index;
  }
  ranges.push_back({first, last - first + 1u});
  return ranges;
}

namespace
{

uint64_t sparseVoxelAlignedBytes(const uint64_t bytes, const uint64_t alignment = 16u)
{
  if (bytes == 0u)
    return 0u;
  return ((bytes + alignment - 1u) / alignment) * alignment;
}

uint64_t sparseVoxelRangeBytes(const SparseVoxelBufferRange range, const uint64_t elementBytes)
{
  return sparseVoxelAlignedBytes(static_cast<uint64_t>(range.count) * elementBytes);
}

bool failProducedPayload(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

} // namespace

uint64_t sparseVoxelPhysicalPagePayloadBytes(const SparseVoxelPhysicalPageAllocation &allocation)
{
  uint64_t bytes = 0u;
  bytes += sparseVoxelRangeBytes(allocation.hierarchyNodes, sizeof(SparseVoxelLODNodeGPU));
  bytes += sparseVoxelRangeBytes(allocation.childIndices, sizeof(uint32_t));
  bytes += sparseVoxelRangeBytes(allocation.brickNodes, sizeof(SparseVoxelBrickNodeGPU));
  bytes += sparseVoxelRangeBytes(allocation.brickOccupancy, sizeof(SparseVoxelBrickOccupancyGPU));
  bytes += sparseVoxelRangeBytes(allocation.materials, sizeof(SparseVoxelMaterialGPU));
  bytes += sparseVoxelRangeBytes(allocation.materialCells, sizeof(SparseVoxelBrickMaterialGPU));
  // Budget the padded Tree64 node shape because the dynamic trace path consumes
  // the vec4<u32> layout even when an offline/raw source starts as 12-byte nodes.
  bytes += sparseVoxelRangeBytes(allocation.tree64Nodes, sizeof(SparseVoxelTree64NodePaddedGPU));
  bytes += sparseVoxelRangeBytes(allocation.tree64Materials, sizeof(SparseVoxelBrickMaterialGPU));
  bytes += sparseVoxelRangeBytes(allocation.traceNodes, sizeof(VoxelTraceNode64GPU));
  bytes += sparseVoxelRangeBytes(allocation.traceErrorWords, sizeof(uint32_t));
  bytes += sparseVoxelRangeBytes(allocation.cullErrorWords, sizeof(uint32_t));
  bytes += sparseVoxelRangeBytes(allocation.triangleClusters, sizeof(SparseVoxelTerrainTriangleCluster));
  bytes += sparseVoxelRangeBytes(allocation.triangleVertices, sizeof(SparseVoxelTerrainTriangleVertex));
  bytes += sparseVoxelRangeBytes(allocation.triangleIndices, sizeof(uint32_t));
  bytes += sparseVoxelRangeBytes(allocation.virtualGeometryMeshlets, sizeof(virtualgeometry::Meshlet));
  bytes += sparseVoxelRangeBytes(allocation.virtualGeometryHierarchyNodes, sizeof(virtualgeometry::VirtualGeometryHierarchy));
  bytes += sparseVoxelRangeBytes(allocation.virtualGeometryGroups, sizeof(uint32_t));
  bytes += sparseVoxelRangeBytes(allocation.virtualGeometryPositionWords, sizeof(uint32_t));
  bytes += sparseVoxelRangeBytes(allocation.virtualGeometryNormalWords, sizeof(uint32_t));
  bytes += sparseVoxelRangeBytes(allocation.virtualGeometryUVWords, sizeof(uint32_t));
  bytes += sparseVoxelRangeBytes(allocation.virtualGeometryIndexBytes, sizeof(uint8_t));
  bytes += sparseVoxelRangeBytes(allocation.virtualGeometryBoneWeightWords, sizeof(uint32_t));
  return bytes;
}

uint32_t scoreSparseVoxelPagePriority(const VirtualSparseVoxelFilePageRecord &page, const SparseVoxelPagePriorityView &view, const bool visible)
{
  auto transformPoint = [](const std::array<float, 16> &matrix, const std::array<float, 3> &point)
  {
    return std::array<float, 3>{
      matrix[0] * point[0] + matrix[4] * point[1] + matrix[8] * point[2] + matrix[12],
      matrix[1] * point[0] + matrix[5] * point[1] + matrix[9] * point[2] + matrix[13],
      matrix[2] * point[0] + matrix[6] * point[1] + matrix[10] * point[2] + matrix[14],
    };
  };
  std::array<float, 2> minimum{std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
  std::array<float, 2> maximum{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
  uint32_t acceptedCorners = 0u;
  for (uint32_t corner = 0u; corner < 8u; ++corner)
  {
    const std::array<float, 3> local{
      (corner & 4u) != 0u ? page.localBoundsMax[0] : page.localBoundsMin[0],
      (corner & 2u) != 0u ? page.localBoundsMax[1] : page.localBoundsMin[1],
      (corner & 1u) != 0u ? page.localBoundsMax[2] : page.localBoundsMin[2],
    };
    const auto world = transformPoint(view.modelToWorld, local);
    const auto camera = transformPoint(view.worldToCamera, world);
    if (!std::isfinite(camera[0]) || !std::isfinite(camera[1]) || !std::isfinite(camera[2]) || camera[2] < view.nearPlane)
      continue;
    const std::array<float, 2> pixel{view.focalPixels[0] * camera[0] / camera[2], view.focalPixels[1] * camera[1] / camera[2]};
    minimum[0] = std::min(minimum[0], pixel[0]);
    minimum[1] = std::min(minimum[1], pixel[1]);
    maximum[0] = std::max(maximum[0], pixel[0]);
    maximum[1] = std::max(maximum[1], pixel[1]);
    ++acceptedCorners;
  }
  const std::array<float, 3> localCenter{
    (page.localBoundsMin[0] + page.localBoundsMax[0]) * 0.5f,
    (page.localBoundsMin[1] + page.localBoundsMax[1]) * 0.5f,
    (page.localBoundsMin[2] + page.localBoundsMax[2]) * 0.5f,
  };
  const auto worldCenter = transformPoint(view.modelToWorld, localCenter);
  const float dx = worldCenter[0] - view.cameraWorldPosition[0];
  const float dy = worldCenter[1] - view.cameraWorldPosition[1];
  const float dz = worldCenter[2] - view.cameraWorldPosition[2];
  const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
  const float projected = acceptedCorners == 0u ? 0.0f : std::max(maximum[0] - minimum[0], maximum[1] - minimum[1]);
  return quantizeSparseVoxelPagePriority(visible && acceptedCorners != 0u, projected, distance);
}

bool sparseVoxelProducedPagePayloadFromPrebuilt(const SparseVoxelPrebuiltPagePackage &package, SparseVoxelProducedPagePayload &payload, std::string *error)
{
  payload = {};
  auto failPackage = [error](const std::string &message)
  {
    if (error != nullptr)
      *error = message;
    return false;
  };
  auto tooLarge = [](const size_t count)
  {
    return count > static_cast<size_t>(std::numeric_limits<uint32_t>::max());
  };

  if (!package.page.valid())
    return failPackage("sparse voxel prebuilt package page id is invalid");
  if (!package.decoded)
    return failPackage("sparse voxel prebuilt package has not been decoded");
  if (package.requiredProducerRevision == 0u)
    return failPackage("sparse voxel prebuilt package producer revision is invalid");
  if (package.payloadRevision == 0u)
    return failPackage("sparse voxel prebuilt package payload revision is invalid");
  if (!package.hasRenderablePayload())
    return failPackage("sparse voxel prebuilt package has no renderable payload");
  if (package.installRewrites.empty())
    return failPackage("sparse voxel prebuilt package requires install rewrites");
  if (tooLarge(package.pageData.nodes.size()) || tooLarge(package.pageData.pathsLevels.size()) || tooLarge(package.pageData.centersLengths.size()) || tooLarge(package.pageData.materials.size()) ||
      tooLarge(package.installRewrites.size()) || tooLarge(package.uninstallRewrites.size()))
  {
    return failPackage("sparse voxel prebuilt package exceeds 32-bit physical buffer counts");
  }

  const auto nodeCount = static_cast<uint32_t>(package.pageData.nodes.size());
  const auto materialCount = static_cast<uint32_t>(package.pageData.materials.size());
  if (nodeCount != 0u)
  {
    if (package.pageData.centersLengths.size() != package.pageData.nodes.size() || package.pageData.pathsLevels.size() != package.pageData.nodes.size() ||
        package.pageData.materials.size() != package.pageData.nodes.size())
    {
      return failPackage("sparse voxel prebuilt package page arrays must match node count");
    }
  }

  payload.page = package.page;
  payload.expectedPageGeneration = package.expectedPageGeneration;
  payload.requiredProducerRevision = package.requiredProducerRevision;
  payload.payloadRevision = package.payloadRevision;
  payload.geometryRevision = package.payloadRevision;
  payload.materialRevision = package.payloadRevision;
  payload.requirements.page = package.page;
  payload.requirements.geometryRevision = package.payloadRevision;
  payload.requirements.materialRevision = package.payloadRevision;
  payload.requirements.traceFormat = package.traceFormat;
  payload.requirements.hierarchyNodes.count = nodeCount;
  payload.requirements.childIndices.count = package.childIndexCount;
  payload.requirements.brickNodes.count = nodeCount;
  payload.requirements.brickOccupancy.count = package.brickOccupancyCount != 0u ? package.brickOccupancyCount : nodeCount;
  payload.requirements.materials.count = materialCount;
  payload.requirements.materialCells.count = package.materialCellCount != 0u ? package.materialCellCount : materialCount;
  payload.requirements.tree64Nodes.count = package.tree64NodeCount;
  payload.requirements.tree64Materials.count = package.tree64MaterialCount;
  payload.requirements.traceNodes.count = package.traceNodeCount;
  payload.requirements.traceErrorWords.count = package.traceErrorWordCount;
  payload.requirements.cullErrorWords.count = package.cullErrorWordCount;
  payload.installRewriteCount = static_cast<uint32_t>(package.installRewrites.size());
  payload.uninstallRewriteCount = static_cast<uint32_t>(package.uninstallRewrites.size());
  payload.payloadComplete = true;
  if (error != nullptr)
    error->clear();
  return true;
}

bool sparseVoxelProducedPagePayloadFromTerrainTriangleSidecar(
    const SparseVoxelTerrainTrianglePageSidecar &sidecar,
    const SparseVoxelTerrainTriangleMeshSettings &settings,
    const SparseVoxelPageId page,
    const uint64_t expectedPageGeneration,
    const uint32_t requiredProducerRevision,
    const uint64_t payloadRevision,
    SparseVoxelProducedPagePayload &payload,
    std::string *error)
{
  payload = {};
  if (!page.valid())
    return failProducedPayload(error, "terrain triangle sidecar page id is invalid");
  if (payloadRevision == 0u)
    return failProducedPayload(error, "terrain triangle sidecar payload revision is invalid");
  if (!validateSparseVoxelTerrainTrianglePageSidecar(sidecar, settings, error))
    return false;
  if (sidecar.empty())
    return failProducedPayload(error, "terrain triangle sidecar has no raster payload");
  if (sidecar.vertices.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max()) ||
      sidecar.indices.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max()) ||
      sidecar.clusters.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
  {
    return failProducedPayload(error, "terrain triangle sidecar exceeds 32-bit physical buffer counts");
  }

  const uint64_t geometryRevision =
      sidecar.tree64Revision != 0u
          ? sidecar.tree64Revision
          : sidecar.brickPayloadRevision;
  if (geometryRevision == 0u)
    return failProducedPayload(error, "terrain triangle sidecar geometry revision is invalid");

  payload.page = page;
  payload.expectedPageGeneration = expectedPageGeneration;
  payload.requiredProducerRevision = requiredProducerRevision;
  payload.payloadRevision = payloadRevision;
  payload.geometryRevision = geometryRevision;
  payload.materialRevision = geometryRevision;
  payload.requirements.page = page;
  payload.requirements.geometryRevision = geometryRevision;
  payload.requirements.materialRevision = geometryRevision;
  payload.requirements.traceFormat = kVoxelTraceFormatInvalid;
  payload.requirements.triangleClusters.count =
      static_cast<uint32_t>(sidecar.clusters.size());
  payload.requirements.triangleVertices.count =
      static_cast<uint32_t>(sidecar.vertices.size());
  payload.requirements.triangleIndices.count =
      static_cast<uint32_t>(sidecar.indices.size());
  payload.installRewriteCount = 0u;
  payload.uninstallRewriteCount = 1u;
  payload.payloadComplete = true;
  if (error != nullptr)
    error->clear();
  return true;
}

bool SparseVoxelPhysicalRangeAllocator::fail(std::string *error, const std::string &message) const
{
  if (error != nullptr)
    *error = message;
  return false;
}

bool SparseVoxelPhysicalRangeAllocator::configure(const uint32_t capacity, std::string *error)
{
  capacity_ = capacity;
  active_ = 0u;
  retiring_ = 0u;
  completedFence_ = 0u;
  activeRanges_.clear();
  retiringRanges_.clear();
  freeRanges_.clear();
  if (capacity != 0u)
    freeRanges_.push_back({0u, capacity});
  if (error != nullptr)
    error->clear();
  return true;
}

bool SparseVoxelPhysicalRangeAllocator::canAllocate(const uint32_t count) const
{
  if (count == 0u)
    return true;
  for (const auto range : freeRanges_)
    if (range.count >= count)
      return true;
  return false;
}

bool SparseVoxelPhysicalRangeAllocator::allocate(const uint32_t count, SparseVoxelBufferRange &range, std::string *error)
{
  range = {};
  if (count == 0u)
  {
    if (error != nullptr)
      error->clear();
    return true;
  }
  for (auto &freeRange : freeRanges_)
  {
    if (freeRange.count < count)
      continue;
    range = {freeRange.first, count};
    freeRange.first += count;
    freeRange.count -= count;
    active_ += count;
    activeRanges_.push_back(range);
    freeRanges_.erase(
        std::remove_if(
            freeRanges_.begin(),
            freeRanges_.end(),
            [](const SparseVoxelBufferRange value)
            {
              return value.count == 0u;
            }),
        freeRanges_.end());
    if (error != nullptr)
      error->clear();
    return true;
  }
  return fail(error, "sparse voxel physical allocator is out of space");
}

bool SparseVoxelPhysicalRangeAllocator::rangeContains(const SparseVoxelBufferRange owner, const SparseVoxelBufferRange range)
{
  if (range.count == 0u)
    return true;
  if (!owner.valid() || !range.valid())
    return false;
  const uint64_t ownerFirst = owner.first;
  const uint64_t ownerEnd = ownerFirst + owner.count;
  const uint64_t rangeFirst = range.first;
  const uint64_t rangeEnd = rangeFirst + range.count;
  return ownerFirst <= rangeFirst && rangeEnd <= ownerEnd;
}

bool SparseVoxelPhysicalRangeAllocator::containsActive(const SparseVoxelBufferRange range) const
{
  if (range.count == 0u)
    return true;
  for (const auto activeRange : activeRanges_)
    if (rangeContains(activeRange, range))
      return true;
  return false;
}

bool SparseVoxelPhysicalRangeAllocator::containsRetiring(const SparseVoxelBufferRange range) const
{
  if (range.count == 0u)
    return false;
  for (const auto retiringRange : retiringRanges_)
    if (rangeContains(retiringRange.range, range))
      return true;
  return false;
}

bool SparseVoxelPhysicalRangeAllocator::retire(const SparseVoxelBufferRange range, const uint64_t completionFence, std::string *error)
{
  if (range.count == 0u)
  {
    if (error != nullptr)
      error->clear();
    return true;
  }
  if (!range.valid() || completionFence < completedFence_)
    return fail(error, "sparse voxel physical retirement range is invalid");
  const auto found = std::find_if(
      activeRanges_.begin(),
      activeRanges_.end(),
      [range](const SparseVoxelBufferRange activeRange)
      {
        return activeRange.first == range.first && activeRange.count == range.count;
      });
  if (found == activeRanges_.end())
    return fail(error, "sparse voxel physical retirement range is not active");
  active_ -= range.count;
  retiring_ += range.count;
  retiringRanges_.push_back({range, completionFence});
  activeRanges_.erase(found);
  if (error != nullptr)
    error->clear();
  return true;
}

void SparseVoxelPhysicalRangeAllocator::insertFree(const SparseVoxelBufferRange range)
{
  if (range.count == 0u)
    return;
  freeRanges_.push_back(range);
  std::sort(
      freeRanges_.begin(),
      freeRanges_.end(),
      [](const SparseVoxelBufferRange a, const SparseVoxelBufferRange b)
      {
        return a.first < b.first;
      });
  std::vector<SparseVoxelBufferRange> merged;
  for (const auto freeRange : freeRanges_)
  {
    if (!freeRange.valid() || freeRange.count == 0u)
      continue;
    if (!merged.empty() && merged.back().first + merged.back().count == freeRange.first)
      merged.back().count += freeRange.count;
    else
      merged.push_back(freeRange);
  }
  freeRanges_ = std::move(merged);
}

uint32_t SparseVoxelPhysicalRangeAllocator::reclaim(const uint64_t completedFence)
{
  completedFence_ = std::max(completedFence_, completedFence);
  uint32_t reclaimed = 0u;
  for (size_t index = 0u; index < retiringRanges_.size();)
  {
    const auto retiring = retiringRanges_[index];
    if (retiring.completionFence > completedFence_)
    {
      ++index;
      continue;
    }
    retiring_ -= retiring.range.count;
    insertFree(retiring.range);
    retiringRanges_.erase(retiringRanges_.begin() + static_cast<std::ptrdiff_t>(index));
    ++reclaimed;
  }
  return reclaimed;
}

SparseVoxelPhysicalAllocatorStats SparseVoxelPhysicalRangeAllocator::stats() const
{
  SparseVoxelPhysicalAllocatorStats result{};
  result.capacity = capacity_;
  result.active = active_;
  result.retiring = retiring_;
  result.activeRangeCount = static_cast<uint32_t>(activeRanges_.size());
  result.retiringRangeCount = static_cast<uint32_t>(retiringRanges_.size());
  for (const auto range : freeRanges_)
    result.largestFreeRange = std::max(result.largestFreeRange, range.count);
  return result;
}

bool SparseVoxelSharedPageTable::fail(std::string *error, const std::string &message) const
{
  if (error != nullptr)
    *error = message;
  return false;
}

SparseVoxelPhysicalRangeAllocator &SparseVoxelSharedPageTable::allocator(const SparseVoxelPhysicalBufferKind kind)
{
  return allocators_[static_cast<uint32_t>(kind)];
}

const SparseVoxelPhysicalRangeAllocator &SparseVoxelSharedPageTable::allocator(const SparseVoxelPhysicalBufferKind kind) const
{
  return allocators_[static_cast<uint32_t>(kind)];
}

bool SparseVoxelSharedPageTable::configure(const SparseVoxelSharedPageTableSettings &settings, std::string *error)
{
  allocator(SparseVoxelPhysicalBufferKind::HierarchyNodes).configure(settings.hierarchyNodeCapacity);
  allocator(SparseVoxelPhysicalBufferKind::ChildIndices).configure(settings.childIndexCapacity);
  allocator(SparseVoxelPhysicalBufferKind::BrickNodes).configure(settings.brickNodeCapacity);
  allocator(SparseVoxelPhysicalBufferKind::BrickOccupancy).configure(settings.brickOccupancyCapacity);
  allocator(SparseVoxelPhysicalBufferKind::Materials).configure(settings.materialCapacity);
  allocator(SparseVoxelPhysicalBufferKind::MaterialCells).configure(settings.materialCellCapacity);
  allocator(SparseVoxelPhysicalBufferKind::Tree64Nodes).configure(settings.tree64NodeCapacity);
  allocator(SparseVoxelPhysicalBufferKind::Tree64Materials).configure(settings.tree64MaterialCapacity);
  allocator(SparseVoxelPhysicalBufferKind::TraceNodes).configure(settings.traceNodeCapacity);
  allocator(SparseVoxelPhysicalBufferKind::TraceErrorWords).configure(settings.traceErrorWordCapacity);
  allocator(SparseVoxelPhysicalBufferKind::CullErrorWords).configure(settings.cullErrorWordCapacity);
  allocator(SparseVoxelPhysicalBufferKind::TriangleClusters).configure(settings.triangleClusterCapacity);
  allocator(SparseVoxelPhysicalBufferKind::TriangleVertices).configure(settings.triangleVertexCapacity);
  allocator(SparseVoxelPhysicalBufferKind::TriangleIndices).configure(settings.triangleIndexCapacity);
  allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryMeshlets).configure(settings.virtualGeometryMeshletCapacity);
  allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryHierarchyNodes).configure(settings.virtualGeometryHierarchyNodeCapacity);
  allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryGroups).configure(settings.virtualGeometryGroupCapacity);
  allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryPositionWords).configure(settings.virtualGeometryPositionWordCapacity);
  allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryNormalWords).configure(settings.virtualGeometryNormalWordCapacity);
  allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryUVWords).configure(settings.virtualGeometryUVWordCapacity);
  allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryIndexBytes).configure(settings.virtualGeometryIndexByteCapacity);
  allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryBoneWeightWords).configure(settings.virtualGeometryBoneWeightWordCapacity);
  configured_ = true;
  completedFence_ = 0u;
  nextAllocationRevision_ = 1u;
  packedPageSlotBytes_ =
      settings.packedPageSlotBytes == 0u
          ? 0u
          : static_cast<uint32_t>(sparseVoxelAlignedBytes(
                std::max<uint32_t>(settings.packedPageSlotBytes, 16u)));
  packedPageSlots_.clear();
  entries_.clear();
  dependencies_.clear();
  dependents_.clear();
  installRewrites_.clear();
  uninstallRewrites_.clear();
  dirtyPageTableRanges_.clear();
  if (error != nullptr)
    error->clear();
  return true;
}

SparseVoxelSharedPageTableEntry *SparseVoxelSharedPageTable::mutableEntry(const SparseVoxelPageId page)
{
  const auto found = std::find_if(
      entries_.begin(),
      entries_.end(),
      [page](const SparseVoxelSharedPageTableEntry &entry)
      {
        return entry.page == page;
      });
  return found == entries_.end() ? nullptr : &*found;
}

void SparseVoxelSharedPageTable::markPageTableDirty(const uint32_t index)
{
  SparseVoxelPageTableDirtyRange merged{index, 1u};
  for (auto it = dirtyPageTableRanges_.begin(); it != dirtyPageTableRanges_.end();)
  {
    const uint64_t leftBegin = merged.first;
    const uint64_t leftEnd = leftBegin + merged.count;
    const uint64_t rightBegin = it->first;
    const uint64_t rightEnd = rightBegin + it->count;
    if (leftEnd < rightBegin || rightEnd < leftBegin)
    {
      ++it;
      continue;
    }
    const uint64_t begin = std::min(leftBegin, rightBegin);
    const uint64_t end = std::max(leftEnd, rightEnd);
    merged.first = static_cast<uint32_t>(begin);
    merged.count = static_cast<uint32_t>(end - begin);
    it = dirtyPageTableRanges_.erase(it);
  }
  dirtyPageTableRanges_.push_back(merged);
  std::sort(
      dirtyPageTableRanges_.begin(),
      dirtyPageTableRanges_.end(),
      [](const auto &left, const auto &right)
      {
        return left.first < right.first;
      });
}

void SparseVoxelSharedPageTable::clearDirtyPageTableRanges()
{
  dirtyPageTableRanges_.clear();
}

const SparseVoxelSharedPageTableEntry *SparseVoxelSharedPageTable::entry(const SparseVoxelPageId page) const
{
  const auto found = std::find_if(
      entries_.begin(),
      entries_.end(),
      [page](const SparseVoxelSharedPageTableEntry &entry)
      {
        return entry.page == page;
      });
  return found == entries_.end() ? nullptr : &*found;
}

bool selectSparseVoxelStreamedInstanceLOD(
    const SparseVoxelSharedPageTable &table,
    const SparseVoxelStreamedInstance &instance,
    const SparseVoxelPageId desiredPage,
    SparseVoxelInstanceLODSelection &selection,
    std::string *error)
{
  selection = {};
  if (!instance.valid())
  {
    if (error != nullptr)
      *error = "sparse voxel streamed instance is invalid";
    return false;
  }
  if (desiredPage.valid() && desiredPage.producer != instance.producer.producerId)
  {
    if (error != nullptr)
      *error = "sparse voxel desired page belongs to a different producer";
    return false;
  }

  const auto *root = table.entry(instance.rootPage);
  if (root == nullptr)
  {
    if (error != nullptr)
      *error = "sparse voxel streamed instance root page is missing";
    return false;
  }

  selection.instanceIndex = instance.instanceIndex;
  selection.desiredPage = desiredPage.valid() ? desiredPage : instance.rootPage;
  selection.rootResident = root->resident();
  if (!selection.rootResident)
  {
    if (error != nullptr)
      *error = "sparse voxel streamed instance root fallback is not resident";
    return false;
  }

  const auto *desired = table.entry(selection.desiredPage);
  selection.desiredResident = desired != nullptr && desired->resident();
  if (selection.desiredPage == instance.rootPage || selection.desiredResident)
  {
    selection.selectedPage = selection.desiredPage;
    selection.selectedFallback = selection.desiredPage == instance.rootPage;
    selection.requestedDesiredPage = false;
  }
  else
  {
    selection.selectedPage = instance.rootPage;
    selection.selectedFallback = true;
    selection.requestedDesiredPage = true;
  }

  if (error != nullptr)
    error->clear();
  return true;
}

namespace
{

bool sparseVoxelEntrySatisfiesUnifiedCut(
    const SparseVoxelSharedPageTableEntry *entry,
    const SparseVoxelUnifiedGeometryCutSettings &settings)
{
  if (entry == nullptr || !entry->resident())
    return false;
  if (settings.requireRasterPayload && !entry->rasterVisible())
    return false;
  if (settings.requireTracePayload && !entry->traceable())
    return false;
  if (settings.requireMaterialPayload && !entry->materialTraceable())
    return false;
  return true;
}

void sparseVoxelFillUnifiedSelectionPageState(
    const SparseVoxelSharedPageTableEntry *entry,
    bool &resident,
    bool &rasterVisible,
    bool &traceable,
    bool &materialTraceable)
{
  resident = entry != nullptr && entry->resident();
  rasterVisible = entry != nullptr && entry->rasterVisible();
  traceable = entry != nullptr && entry->traceable();
  materialTraceable = entry != nullptr && entry->materialTraceable();
}

} // namespace

bool selectSparseVoxelUnifiedGeometryLOD(
    const SparseVoxelSharedPageTable &table,
    const SparseVoxelStreamedInstance &instance,
    const SparseVoxelPageId desiredPage,
    const SparseVoxelUnifiedGeometryCutSettings &settings,
    SparseVoxelUnifiedGeometryLODSelection &selection,
    std::string *error)
{
  selection = {};
  if (!instance.valid())
  {
    if (error != nullptr)
      *error = "sparse voxel unified geometry instance is invalid";
    return false;
  }
  if (desiredPage.valid() && desiredPage.producer != instance.producer.producerId)
  {
    if (error != nullptr)
      *error = "sparse voxel unified geometry desired page belongs to a different producer";
    return false;
  }

  const SparseVoxelPageId resolvedDesired =
      desiredPage.valid() ? desiredPage : instance.rootPage;
  const auto *root = table.entry(instance.rootPage);
  const auto *desired = table.entry(resolvedDesired);

  selection.instanceIndex = instance.instanceIndex;
  selection.desiredPage = resolvedDesired;
  sparseVoxelFillUnifiedSelectionPageState(
      root,
      selection.rootResident,
      selection.rootRasterVisible,
      selection.rootTraceable,
      selection.rootMaterialTraceable);
  sparseVoxelFillUnifiedSelectionPageState(
      desired,
      selection.desiredResident,
      selection.desiredRasterVisible,
      selection.desiredTraceable,
      selection.desiredMaterialTraceable);

  if (root == nullptr)
  {
    if (error != nullptr)
      *error = "sparse voxel unified geometry root page is missing";
    return false;
  }
  if (!root->resident())
  {
    if (error != nullptr)
      *error = "sparse voxel unified geometry root fallback is not resident";
    return false;
  }
  if (!sparseVoxelEntrySatisfiesUnifiedCut(root, settings))
  {
    if (error != nullptr)
      *error = "sparse voxel unified geometry root fallback does not satisfy required payload views";
    return false;
  }

  const bool desiredSatisfies =
      sparseVoxelEntrySatisfiesUnifiedCut(desired, settings);
  selection.missingRasterPayload =
      settings.requireRasterPayload && !selection.desiredRasterVisible;
  selection.missingTracePayload =
      settings.requireTracePayload && !selection.desiredTraceable;
  selection.missingMaterialPayload =
      settings.requireMaterialPayload && !selection.desiredMaterialTraceable;

  if (resolvedDesired == instance.rootPage || desiredSatisfies)
  {
    selection.selectedPage = resolvedDesired;
    selection.selectedFallback = resolvedDesired == instance.rootPage;
    selection.requestedDesiredPage = false;
  }
  else
  {
    if (!settings.allowRootFallback)
    {
      if (error != nullptr)
        *error = "sparse voxel unified geometry desired page is not ready and fallback is disabled";
      return false;
    }
    selection.selectedPage = instance.rootPage;
    selection.selectedFallback = true;
    selection.requestedDesiredPage = true;
  }

  const auto *selected = table.entry(selection.selectedPage);
  selection.selectedRasterVisible = selected != nullptr && selected->rasterVisible();
  selection.selectedTraceable = selected != nullptr && selected->traceable();
  selection.selectedMaterialTraceable = selected != nullptr && selected->materialTraceable();
  if (error != nullptr)
    error->clear();
  return true;
}

bool traverseSparseVoxelStreamedInstancesLOD(
    const SparseVoxelSharedPageTable &table,
    const std::vector<SparseVoxelTraversalCandidate> &candidates,
    const SparseVoxelPagePriorityView &view,
    SparseVoxelTraversalFrameResult &result,
    std::string *error)
{
  result = {};
  for (const auto &candidate : candidates)
  {
    SparseVoxelInstanceLODSelection selection{};
    if (!selectSparseVoxelStreamedInstanceLOD(table, candidate.instance, candidate.desiredPage, selection, error))
      return false;

    result.emissions.push_back({selection.instanceIndex, selection.selectedPage, selection.selectedFallback});
    if (selection.selectedFallback)
      result.diagnostics.parentFallbackCount += 1u;
    else
      result.diagnostics.childSelectionCount += 1u;

    if (selection.requestedDesiredPage)
    {
      const uint32_t priority = scoreSparseVoxelPagePriority(candidate.desiredPageBounds, view, candidate.visible);
      std::vector<SparseVoxelPageId> requestClosure;
      if (!table.collectPageRequestClosure(selection.desiredPage, requestClosure, error))
        return false;
      for (const auto requestPage : requestClosure)
        result.pageRequests.push_back({selection.instanceIndex, requestPage, priority, candidate.visible});
      result.diagnostics.pageResidencyMissCount += 1u;
    }
  }

  std::sort(
      result.pageRequests.begin(),
      result.pageRequests.end(),
      [](const SparseVoxelTraversalPageRequest &left, const SparseVoxelTraversalPageRequest &right)
      {
        if (left.priority != right.priority)
          return left.priority > right.priority;
        if (left.page == right.page)
          return left.instanceIndex < right.instanceIndex;
        return left.page < right.page;
      });

  if (error != nullptr)
    error->clear();
  return true;
}

bool traverseSparseVoxelUnifiedGeometryLOD(
    const SparseVoxelSharedPageTable &table,
    const std::vector<SparseVoxelTraversalCandidate> &candidates,
    const SparseVoxelPagePriorityView &view,
    const SparseVoxelUnifiedGeometryCutSettings &settings,
    SparseVoxelUnifiedGeometryTraversalResult &result,
    std::string *error)
{
  result = {};
  result.counters.inputCandidateCount =
      static_cast<uint32_t>(candidates.size());

  for (const auto &candidate : candidates)
  {
    SparseVoxelUnifiedGeometryLODSelection selection{};
    if (!selectSparseVoxelUnifiedGeometryLOD(
            table,
            candidate.instance,
            candidate.desiredPage,
            settings,
            selection,
            error))
    {
      return false;
    }

    result.selections.push_back(selection);
    if (selection.selectedFallback)
    {
      ++result.counters.selectedFallbackCount;
      const auto *selected = table.entry(selection.selectedPage);
      if (selected != nullptr && selected->allocation.hasBrickRasterPayload())
        ++result.counters.brickFallbackPageCount;
    }
    else
    {
      ++result.counters.selectedChildCount;
    }
    if (selection.missingRasterPayload)
      ++result.counters.missingRasterPageCount;
    if (selection.missingTracePayload)
      ++result.counters.missingTracePageCount;
    if (selection.missingMaterialPayload)
      ++result.counters.missingMaterialPageCount;

    if (selection.requestedDesiredPage)
    {
      const uint32_t priority =
          scoreSparseVoxelPagePriority(
              candidate.desiredPageBounds,
              view,
              candidate.visible);
      std::vector<SparseVoxelPageId> requestClosure;
      if (!table.collectPageRequestClosure(selection.desiredPage, requestClosure, error))
        return false;
      if (std::find(requestClosure.begin(), requestClosure.end(), selection.desiredPage) == requestClosure.end())
        requestClosure.push_back(selection.desiredPage);
      for (const auto requestPage : requestClosure)
        result.pageRequests.push_back({selection.instanceIndex, requestPage, priority, candidate.visible});
      result.counters.requestedPageCount += static_cast<uint32_t>(requestClosure.size());
      if (!candidate.visible)
        result.counters.offscreenRequestCount += static_cast<uint32_t>(requestClosure.size());
    }
  }

  std::sort(
      result.pageRequests.begin(),
      result.pageRequests.end(),
      [](const SparseVoxelTraversalPageRequest &left, const SparseVoxelTraversalPageRequest &right)
      {
        if (left.priority != right.priority)
          return left.priority > right.priority;
        if (left.page == right.page)
          return left.instanceIndex < right.instanceIndex;
        return left.page < right.page;
      });
  result.pageRequests.erase(
      std::unique(
          result.pageRequests.begin(),
          result.pageRequests.end(),
          [](const SparseVoxelTraversalPageRequest &left, const SparseVoxelTraversalPageRequest &right)
          {
            return left.page == right.page &&
                left.instanceIndex == right.instanceIndex &&
                left.visible == right.visible;
          }),
      result.pageRequests.end());
  result.counters.requestedPageCount =
      static_cast<uint32_t>(result.pageRequests.size());

  for (const auto &entry : table.debugReadback())
  {
    if (!entry.resident())
      continue;
    if (entry.allocation.hasTriangleRasterPayload())
    {
      ++result.counters.triangleRasterPageCount;
      result.counters.triangleSidecarBytes +=
          sparseVoxelRangeBytes(entry.allocation.triangleClusters, sizeof(SparseVoxelTerrainTriangleCluster)) +
          sparseVoxelRangeBytes(entry.allocation.triangleVertices, sizeof(SparseVoxelTerrainTriangleVertex)) +
          sparseVoxelRangeBytes(entry.allocation.triangleIndices, sizeof(uint32_t));
    }
    if (entry.traceable())
      ++result.counters.tracePageCount;
    const auto eviction = table.evictionDecision(entry.page);
    if (entry.resident() && !entry.pinned && !eviction.evictable && eviction.liveDependentCount != 0u)
      ++result.counters.protectedParentPageCount;
  }

  if (error != nullptr)
    error->clear();
  return true;
}

namespace
{

uint32_t lo32(const uint64_t value)
{
  return static_cast<uint32_t>(value & 0xffff'ffffull);
}

uint32_t hi32(const uint64_t value)
{
  return static_cast<uint32_t>(value >> 32u);
}

uint64_t join32(const uint32_t lo, const uint32_t hi)
{
  return uint64_t{lo} | (uint64_t{hi} << 32u);
}

float finiteOrZero(const float value)
{
  return std::isfinite(value) ? value : 0.0f;
}

} // namespace

void SparseVoxelTraceRequestQueue::configure(const uint32_t capacity)
{
  capacity_ = capacity;
  clear();
}

void SparseVoxelTraceRequestQueue::clear()
{
  requests_.clear();
  stats_ = {};
}

bool SparseVoxelTraceRequestQueue::validRequest(const SparseVoxelTracePageRequestGPU &request)
{
  const SparseVoxelPageId page = sparseVoxelTraceRequestPageId(request);
  return page.valid() && request.rayClass != static_cast<uint32_t>(SparseVoxelTraceRayClass::Unknown);
}

SparseVoxelTracePageRequestKey SparseVoxelTraceRequestQueue::keyFor(const SparseVoxelTracePageRequestGPU &request)
{
  SparseVoxelTracePageRequestKey key{};
  key.page = sparseVoxelTraceRequestPageId(request);
  key.desiredLOD = request.desiredLOD;
  key.rayClass = static_cast<SparseVoxelTraceRayClass>(request.rayClass);
  return key;
}

bool SparseVoxelTraceRequestQueue::lessPriority(
    const SparseVoxelTracePageRequestGPU &left,
    const SparseVoxelTracePageRequestGPU &right)
{
  if (left.priority != right.priority)
    return left.priority < right.priority;
  const auto leftPage = sparseVoxelTraceRequestPageId(left);
  const auto rightPage = sparseVoxelTraceRequestPageId(right);
  if (leftPage == rightPage)
  {
    if (left.desiredLOD != right.desiredLOD)
      return left.desiredLOD > right.desiredLOD;
    return left.rayClass > right.rayClass;
  }
  return rightPage < leftPage;
}

void SparseVoxelTraceRequestQueue::recordEmission(const SparseVoxelTracePageRequestGPU &request)
{
  ++stats_.emitted;
  if ((request.flags & kSparseVoxelTraceRequestRasterFeedback) != 0u || request.rayClass == static_cast<uint32_t>(SparseVoxelTraceRayClass::RasterFeedback))
    ++stats_.rasterRequests;
  else if ((request.flags & kSparseVoxelTraceRequestStabilityFeedback) != 0u || request.rayClass == static_cast<uint32_t>(SparseVoxelTraceRayClass::StabilityFeedback))
    ++stats_.stabilityRequests;
  else
    ++stats_.traceRequests;
  if ((request.flags & kSparseVoxelTraceRequestOffscreen) != 0u)
    ++stats_.offscreenRequests;
  if ((request.flags & kSparseVoxelTraceRequestConservativeFallback) != 0u)
    ++stats_.fallbackTraceCount;
  if ((request.flags & kSparseVoxelTraceRequestExactUnresolved) != 0u)
    ++stats_.unresolvedExactCount;
}

SparseVoxelTraceRequestQueueStatus SparseVoxelTraceRequestQueue::emit(
    const SparseVoxelTracePageRequestGPU &request,
    std::string *error)
{
  if (!validRequest(request))
  {
    if (error != nullptr)
      *error = "sparse voxel trace page request is invalid";
    return SparseVoxelTraceRequestQueueStatus::Invalid;
  }

  recordEmission(request);
  const auto key = keyFor(request);
  for (auto &existing : requests_)
  {
    if (!(keyFor(existing) == key))
      continue;
    existing.priority = std::max(existing.priority, request.priority);
    existing.flags |= request.flags;
    if (std::isfinite(request.desiredError) && (!std::isfinite(existing.desiredError) || request.desiredError < existing.desiredError))
      existing.desiredError = request.desiredError;
    existing.rayConeWidth = std::max(finiteOrZero(existing.rayConeWidth), finiteOrZero(request.rayConeWidth));
    existing.minimumT = std::min(finiteOrZero(existing.minimumT), finiteOrZero(request.minimumT));
    existing.maximumT = std::max(finiteOrZero(existing.maximumT), finiteOrZero(request.maximumT));
    ++stats_.deduplicated;
    if (error != nullptr)
      error->clear();
    return SparseVoxelTraceRequestQueueStatus::Deduplicated;
  }

  if (requests_.size() < capacity_)
  {
    requests_.push_back(request);
    ++stats_.accepted;
    if (error != nullptr)
      error->clear();
    return SparseVoxelTraceRequestQueueStatus::Accepted;
  }

  ++stats_.overflowed;
  auto lowest = std::min_element(requests_.begin(), requests_.end(), lessPriority);
  if (lowest != requests_.end() && lessPriority(*lowest, request))
  {
    *lowest = request;
    if (error != nullptr)
      error->clear();
    return SparseVoxelTraceRequestQueueStatus::ReplacedLowerPriority;
  }
  if (error != nullptr)
    error->clear();
  return SparseVoxelTraceRequestQueueStatus::DroppedOverflow;
}

std::vector<SparseVoxelTracePageRequestGPU> SparseVoxelTraceRequestQueue::sortedRequests() const
{
  auto result = requests_;
  std::sort(
      result.begin(),
      result.end(),
      [](const SparseVoxelTracePageRequestGPU &left, const SparseVoxelTracePageRequestGPU &right)
      {
        return SparseVoxelTraceRequestQueue::lessPriority(right, left);
      });
  return result;
}

SparseVoxelTracePageRequestGPU makeSparseVoxelTracePageRequestGPU(
    const SparseVoxelPageId page,
    const uint32_t desiredLOD,
    const SparseVoxelTraceRayClass rayClass,
    const SparseVoxelPageId receiverPage,
    const uint64_t receiverGeneration,
    const uint32_t flags,
    const uint32_t priority,
    const float desiredError,
    const float rayConeWidth,
    const float minimumT,
    const float maximumT)
{
  SparseVoxelTracePageRequestGPU request{};
  request.producerIdLo = lo32(page.producer);
  request.producerIdHi = hi32(page.producer);
  request.pageIdLo = lo32(page.localPage);
  request.pageIdHi = hi32(page.localPage);
  request.desiredLOD = desiredLOD;
  request.rayClass = static_cast<uint32_t>(rayClass);
  request.receiverPageIdLo = lo32(receiverPage.localPage);
  request.receiverPageIdHi = hi32(receiverPage.localPage);
  request.receiverGenerationLo = lo32(receiverGeneration);
  request.receiverGenerationHi = hi32(receiverGeneration);
  request.flags = flags;
  request.priority = priority;
  request.desiredError = finiteOrZero(desiredError);
  request.rayConeWidth = finiteOrZero(rayConeWidth);
  request.minimumT = finiteOrZero(minimumT);
  request.maximumT = finiteOrZero(maximumT);
  return request;
}

SparseVoxelTraceReceiverGPU makeSparseVoxelTraceReceiverGPU(
    const SparseVoxelPageId selectedPage,
    const uint64_t selectedGeneration,
    const uint32_t selectedLOD,
    const bool selectedFallback)
{
  SparseVoxelTraceReceiverGPU receiver{};
  receiver.producerIdLo = lo32(selectedPage.producer);
  receiver.producerIdHi = hi32(selectedPage.producer);
  receiver.selectedPageIdLo = lo32(selectedPage.localPage);
  receiver.selectedPageIdHi = hi32(selectedPage.localPage);
  receiver.selectedGenerationLo = lo32(selectedGeneration);
  receiver.selectedGenerationHi = hi32(selectedGeneration);
  receiver.selectedLOD = selectedLOD;
  receiver.flags = kSparseVoxelTraceReceiverValid |
      (selectedFallback ? kSparseVoxelTraceReceiverSelectedFallback : 0u);
  return receiver;
}

SparseVoxelPageId sparseVoxelTraceRequestPageId(const SparseVoxelTracePageRequestGPU &request)
{
  return {join32(request.producerIdLo, request.producerIdHi), join32(request.pageIdLo, request.pageIdHi)};
}

SparseVoxelPageId sparseVoxelTraceRequestReceiverPageId(
    const SparseVoxelTracePageRequestGPU &request,
    const uint64_t producer)
{
  return {producer, join32(request.receiverPageIdLo, request.receiverPageIdHi)};
}

uint64_t sparseVoxelTraceRequestReceiverGeneration(const SparseVoxelTracePageRequestGPU &request)
{
  return join32(request.receiverGenerationLo, request.receiverGenerationHi);
}

SparseVoxelPageId sparseVoxelTraceReceiverPageId(const SparseVoxelTraceReceiverGPU &receiver)
{
  return {join32(receiver.producerIdLo, receiver.producerIdHi), join32(receiver.selectedPageIdLo, receiver.selectedPageIdHi)};
}

uint64_t sparseVoxelTraceReceiverGeneration(const SparseVoxelTraceReceiverGPU &receiver)
{
  return join32(receiver.selectedGenerationLo, receiver.selectedGenerationHi);
}

bool processSparseVoxelTraceFeedback(
    const SparseVoxelSharedPageTable &table,
    const SparseVoxelTraceFeedbackInput &input,
    SparseVoxelTraceRequestQueue &requests,
    SparseVoxelTraceFeedbackResult &result,
    std::string *error)
{
  result = {};
  if (!input.instance.valid())
  {
    if (error != nullptr)
      *error = "sparse voxel trace feedback instance is invalid";
    return false;
  }
  const SparseVoxelPageId desiredPage = input.desiredPage.valid() ? input.desiredPage : input.instance.rootPage;
  if (desiredPage.producer != input.instance.producer.producerId)
  {
    if (error != nullptr)
      *error = "sparse voxel trace feedback desired page belongs to a different producer";
    return false;
  }

  const auto *root = table.entry(input.instance.rootPage);
  if (root == nullptr || !root->resident())
  {
    if (error != nullptr)
      *error = "sparse voxel trace feedback root fallback is not resident";
    return false;
  }
  if (input.requireDirectTraceView && !root->traceable())
  {
    if (error != nullptr)
      *error = "sparse voxel trace feedback root fallback has no direct trace view";
    return false;
  }

  result.instanceIndex = input.instance.instanceIndex;
  result.desiredPage = desiredPage;

  const auto *desired = table.entry(desiredPage);
  result.desiredResident = desired != nullptr && desired->resident();
  result.desiredTraceable = desired != nullptr && desired->traceable();
  const bool desiredSelectable = input.requireDirectTraceView
      ? result.desiredTraceable
      : result.desiredResident;
  if (desiredPage == input.instance.rootPage || desiredSelectable)
  {
    result.selectedPage = desiredPage;
    if (error != nullptr)
      error->clear();
    return true;
  }

  uint32_t flags = input.offscreen ? kSparseVoxelTraceRequestOffscreen : kSparseVoxelTraceRequestVisibleReceiver;
  if (input.mode == SparseVoxelTraceLODMode::ConservativeVisibility || input.allowCoarseExactFallback)
    flags |= kSparseVoxelTraceRequestConservativeFallback;
  else
    flags |= kSparseVoxelTraceRequestExactUnresolved;

  const auto status = requests.emit(
      makeSparseVoxelTracePageRequestGPU(
          desiredPage,
          input.desiredLOD,
          input.rayClass,
          input.receiverPage,
          input.receiverGeneration,
          flags,
          input.priority,
          input.desiredError,
          input.rayConeWidth,
          input.minimumT,
          input.maximumT),
      error);
  if (status == SparseVoxelTraceRequestQueueStatus::Invalid)
    return false;
  result.emittedRequest = status == SparseVoxelTraceRequestQueueStatus::Accepted || status == SparseVoxelTraceRequestQueueStatus::Deduplicated || status == SparseVoxelTraceRequestQueueStatus::ReplacedLowerPriority;
  result.requestOverflowed = status == SparseVoxelTraceRequestQueueStatus::DroppedOverflow || status == SparseVoxelTraceRequestQueueStatus::ReplacedLowerPriority;

  if (input.mode == SparseVoxelTraceLODMode::ConservativeVisibility || input.allowCoarseExactFallback)
  {
    result.selectedPage = input.instance.rootPage;
    result.selectedConservativeFallback = true;
  }
  else
  {
    result.unresolvedExact = true;
  }

  if (error != nullptr)
    error->clear();
  return true;
}

bool evaluateSparseVoxelTraceCutConsistency(
    const SparseVoxelSharedPageTable &table,
    const SparseVoxelTraceReceiverGPU &receiver,
    const SparseVoxelTraceFeedbackInput &input,
    SparseVoxelTraceRequestQueue &requests,
    SparseVoxelTraceCutDiagnostics &diagnostics,
    std::string *error)
{
  diagnostics = {};
  diagnostics.screenSelectedPage = sparseVoxelTraceReceiverPageId(receiver);
  diagnostics.screenSelectedGeneration = sparseVoxelTraceReceiverGeneration(receiver);
  diagnostics.screenSelectedLOD = receiver.selectedLOD;
  diagnostics.receiverGeneration = diagnostics.screenSelectedGeneration;
  diagnostics.receiverFallback = (receiver.flags & kSparseVoxelTraceReceiverSelectedFallback) != 0u;
  diagnostics.traceDesiredPage = input.desiredPage.valid() ? input.desiredPage : input.instance.rootPage;
  diagnostics.traceOffscreen = input.offscreen;
  diagnostics.receiverValid = (receiver.flags & kSparseVoxelTraceReceiverValid) != 0u &&
      diagnostics.screenSelectedPage.valid();
  if (!diagnostics.receiverValid)
  {
    if (error != nullptr)
      *error = "sparse voxel trace receiver metadata is invalid";
    return false;
  }
  if (input.receiverPage.valid() && !(input.receiverPage == diagnostics.screenSelectedPage))
  {
    if (error != nullptr)
      *error = "sparse voxel trace receiver page does not match feedback input";
    return false;
  }

  const auto *receiverEntry = table.entry(diagnostics.screenSelectedPage);
  diagnostics.receiverGenerationMatches =
      receiverEntry != nullptr &&
      receiverEntry->resident() &&
      receiverEntry->generation == diagnostics.screenSelectedGeneration;
  if (!diagnostics.receiverGenerationMatches)
  {
    if (error != nullptr)
      *error = "sparse voxel trace receiver generation is stale";
    return false;
  }

  SparseVoxelTraceFeedbackInput resolved = input;
  resolved.receiverPage = diagnostics.screenSelectedPage;
  resolved.receiverGeneration = diagnostics.screenSelectedGeneration;
  SparseVoxelTraceFeedbackResult feedback{};
  if (!processSparseVoxelTraceFeedback(table, resolved, requests, feedback, error))
    return false;

  diagnostics.traceSelectedPage = feedback.selectedPage;
  diagnostics.traceFallbackPage = feedback.selectedConservativeFallback ? input.instance.rootPage : SparseVoxelPageId{};
  diagnostics.traceRequestPage = feedback.emittedRequest ? feedback.desiredPage : SparseVoxelPageId{};
  diagnostics.traceConservativeFallback = feedback.selectedConservativeFallback;
  diagnostics.traceExactUnresolved = feedback.unresolvedExact;
  diagnostics.traceRequestEmitted = feedback.emittedRequest;
  diagnostics.traceRequestOverflowed = feedback.requestOverflowed;
  if (error != nullptr)
    error->clear();
  return true;
}

const std::vector<SparseVoxelPageId> *SparseVoxelSharedPageTable::dependenciesFor(const SparseVoxelPageId page) const
{
  for (size_t index = 0u; index < entries_.size(); ++index)
    if (entries_[index].page == page)
      return &dependencies_[index];
  return nullptr;
}

const std::vector<SparseVoxelPageId> *SparseVoxelSharedPageTable::dependentsFor(const SparseVoxelPageId page) const
{
  for (size_t index = 0u; index < entries_.size(); ++index)
    if (entries_[index].page == page)
      return &dependents_[index];
  return nullptr;
}

bool SparseVoxelSharedPageTable::addPage(const SparseVoxelPageId page, const SparseVoxelPageProducer producer, const std::vector<SparseVoxelPageId> &dependencies, const bool pinned, std::string *error)
{
  if (!configured_)
    return fail(error, "sparse voxel shared page table is not configured");
  if (!page.valid() || !producer.valid() || page.producer != producer.producerId)
    return fail(error, "sparse voxel shared page identity is invalid");
  if (entry(page) != nullptr)
    return fail(error, "sparse voxel shared page already exists");
  for (const auto dependency : dependencies)
    if (entry(dependency) == nullptr)
      return fail(error, "sparse voxel shared page dependency is missing");

  SparseVoxelSharedPageTableEntry record{};
  record.page = page;
  record.producer = producer;
  record.dependencyCount = static_cast<uint32_t>(dependencies.size());
  record.pinned = pinned;
  entries_.push_back(record);
  dependencies_.push_back(dependencies);
  dependents_.push_back({});

  for (const auto dependency : dependencies)
  {
    for (size_t index = 0u; index < entries_.size(); ++index)
    {
      if (entries_[index].page == dependency)
      {
        dependents_[index].push_back(page);
        if (entries_[index].resident())
          entries_.back().residentDependencyCount += 1u;
        break;
      }
    }
  }
  if (error != nullptr)
    error->clear();
  return true;
}

SparseVoxelPackedPageSlotDecision SparseVoxelSharedPageTable::packedPageSlotDecision(const uint32_t slot) const
{
  SparseVoxelPackedPageSlotDecision decision{};
  decision.slot = slot;
  decision.enabled = packedPageSlotBytes_ != 0u;
  decision.slotBytes = packedPageSlotBytes_;
  if (!decision.enabled || slot >= packedPageSlots_.size())
    return decision;

  decision.known = true;
  decision.usedBytes = packedPageSlots_[slot].usedBytes;
  decision.pageletCount = static_cast<uint32_t>(packedPageSlots_[slot].pagelets.size());
  for (const auto &record : entries_)
  {
    if (record.allocation.packedSlot != slot ||
        record.allocation.packedPagelet == kSparseVoxelInvalidPackedPagelet)
      continue;
    if (isSparseVoxelPageLiveOrInFlight(record.state))
    {
      ++decision.livePageletCount;
      decision.liveDependentCount += evictionDecision(record.page).liveDependentCount;
      decision.residentDependentCount += record.residentDependentCount;
    }
    if (record.resident())
      ++decision.residentPageletCount;
    if (record.retiring())
      ++decision.retiringPageletCount;
  }
  decision.reusable =
      decision.livePageletCount == 0u &&
      decision.residentPageletCount == 0u &&
      decision.retiringPageletCount == 0u &&
      decision.liveDependentCount == 0u &&
      decision.residentDependentCount == 0u;
  return decision;
}

bool SparseVoxelSharedPageTable::canReusePackedPageSlot(
    const uint32_t slot,
    SparseVoxelPackedPageSlotDecision *outDecision,
    std::string *error) const
{
  const auto decision = packedPageSlotDecision(slot);
  if (outDecision != nullptr)
    *outDecision = decision;
  if (!decision.enabled)
    return fail(error, "sparse voxel packed page slots are disabled");
  if (!decision.known)
    return fail(error, "sparse voxel packed page slot is missing");
  if (!decision.reusable)
    return fail(error, "sparse voxel packed page slot still contains live pagelets");
  if (error != nullptr)
    error->clear();
  return true;
}

bool SparseVoxelSharedPageTable::collectPageRequestClosure(
    const SparseVoxelPageId page,
    std::vector<SparseVoxelPageId> &pages,
    std::string *error) const
{
  pages.clear();
  if (!configured_)
    return fail(error, "sparse voxel shared page table is not configured");
  std::set<SparseVoxelPageId> visited;
  const auto visit = [&](const auto &self, const SparseVoxelPageId current) -> bool
  {
    if (!current.valid())
      return fail(error, "sparse voxel page request closure contains an invalid page");
    if (!visited.insert(current).second)
      return true;
    const auto *record = entry(current);
    if (record == nullptr)
      return fail(error, "sparse voxel page request closure references a missing page");
    const auto *dependencies = dependenciesFor(current);
    if (dependencies == nullptr)
      return fail(error, "sparse voxel page request closure dependency directory is missing");
    for (const auto dependency : *dependencies)
      if (!self(self, dependency))
        return false;
    if (!record->resident())
      pages.push_back(current);
    return true;
  };
  if (!visit(visit, page))
    return false;
  if (error != nullptr)
    error->clear();
  return true;
}

bool SparseVoxelSharedPageTable::assignPackedPageSlot(
    SparseVoxelPhysicalPageAllocation &allocation,
    const uint64_t payloadBytes,
    std::string *error)
{
  allocation.packedSlot = kSparseVoxelInvalidPackedPageSlot;
  allocation.packedPagelet = kSparseVoxelInvalidPackedPagelet;
  allocation.packedSlotOffsetBytes = 0u;
  allocation.packedSlotBytes = 0u;
  if (packedPageSlotBytes_ == 0u || payloadBytes == 0u)
  {
    if (error != nullptr)
      error->clear();
    return true;
  }
  const uint64_t alignedBytes = sparseVoxelAlignedBytes(payloadBytes);
  if (alignedBytes > packedPageSlotBytes_ ||
      alignedBytes > std::numeric_limits<uint32_t>::max())
  {
    return fail(error, "sparse voxel produced pagelet exceeds packed physical slot size");
  }

  for (uint32_t slot = 0u; slot < packedPageSlots_.size(); ++slot)
  {
    auto decision = packedPageSlotDecision(slot);
    if (decision.reusable)
    {
      packedPageSlots_[slot].usedBytes = 0u;
      packedPageSlots_[slot].pagelets.clear();
      decision.usedBytes = 0u;
      decision.pageletCount = 0u;
    }
    if (packedPageSlots_[slot].usedBytes + alignedBytes <= packedPageSlotBytes_)
    {
      allocation.packedSlot = slot;
      allocation.packedPagelet =
          static_cast<uint32_t>(packedPageSlots_[slot].pagelets.size());
      allocation.packedSlotOffsetBytes = packedPageSlots_[slot].usedBytes;
      allocation.packedSlotBytes = static_cast<uint32_t>(alignedBytes);
      packedPageSlots_[slot].pagelets.push_back(allocation.page);
      packedPageSlots_[slot].usedBytes += static_cast<uint32_t>(alignedBytes);
      if (error != nullptr)
        error->clear();
      return true;
    }
  }

  PackedPageSlot slot{};
  slot.usedBytes = static_cast<uint32_t>(alignedBytes);
  slot.pagelets.push_back(allocation.page);
  allocation.packedSlot = static_cast<uint32_t>(packedPageSlots_.size());
  allocation.packedPagelet = 0u;
  allocation.packedSlotOffsetBytes = 0u;
  allocation.packedSlotBytes = static_cast<uint32_t>(alignedBytes);
  packedPageSlots_.push_back(std::move(slot));
  if (error != nullptr)
    error->clear();
  return true;
}

bool SparseVoxelSharedPageTable::allocatePage(const SparseVoxelPageId page, const SparseVoxelPhysicalPageAllocation &requirements, SparseVoxelPhysicalPageAllocation &allocation, std::string *error)
{
  allocation = {};
  auto *record = mutableEntry(page);
  if (record == nullptr)
    return fail(error, "sparse voxel shared page is missing");
  if (record->resident() || record->retiring() || record->allocation.hasRenderablePayload())
    return fail(error, "sparse voxel shared page already has a live allocation");
  const bool tracePayload = requirements.traceNodes.count != 0u || requirements.traceErrorWords.count != 0u;
  const bool trianglePayload = requirements.triangleClusters.count != 0u || requirements.triangleVertices.count != 0u || requirements.triangleIndices.count != 0u;
  const bool virtualGeometryPayload =
      requirements.virtualGeometryMeshlets.count != 0u ||
      requirements.virtualGeometryHierarchyNodes.count != 0u ||
      requirements.virtualGeometryGroups.count != 0u ||
      requirements.virtualGeometryPositionWords.count != 0u ||
      requirements.virtualGeometryNormalWords.count != 0u ||
      requirements.virtualGeometryUVWords.count != 0u ||
      requirements.virtualGeometryIndexBytes.count != 0u ||
      requirements.virtualGeometryBoneWeightWords.count != 0u;
  if (tracePayload)
  {
    const bool traceFormatValid = requirements.traceFormat == kVoxelTraceFormatCubic64 || requirements.traceFormat == kVoxelTraceFormatBounds8 || requirements.traceFormat == kVoxelTraceFormatBounds16;
    if (!traceFormatValid || requirements.geometryRevision == 0u || requirements.traceNodes.count == 0u)
      return fail(error, "sparse voxel shared page trace allocation identity is invalid");
  }
  if (trianglePayload && (requirements.geometryRevision == 0u || requirements.triangleClusters.count == 0u || requirements.triangleVertices.count == 0u || requirements.triangleIndices.count == 0u))
    return fail(error, "sparse voxel shared page triangle sidecar identity is invalid");
  if (virtualGeometryPayload && (requirements.geometryRevision == 0u || !requirements.requiresVirtualGeometryRasterPayload()))
    return fail(error, "sparse voxel shared page virtual-geometry raster identity is invalid");
  if (requirements.cullErrorWords.count != 0u && requirements.geometryRevision == 0u)
    return fail(error, "sparse voxel shared page cull sidecar geometry revision is invalid");
  if (requirements.materialRevision != 0u && requirements.geometryRevision != 0u && requirements.materialRevision != requirements.geometryRevision)
    return fail(error, "sparse voxel shared page material revision does not match geometry revision");
  const uint64_t payloadBytes = sparseVoxelPhysicalPagePayloadBytes(requirements);
  if (packedPageSlotBytes_ != 0u && payloadBytes > packedPageSlotBytes_)
    return fail(error, "sparse voxel shared page allocation exceeds packed physical slot size");
  if ((requirements.hierarchyNodes.count != 0u && !allocator(SparseVoxelPhysicalBufferKind::HierarchyNodes).canAllocate(requirements.hierarchyNodes.count)) ||
      (requirements.childIndices.count != 0u && !allocator(SparseVoxelPhysicalBufferKind::ChildIndices).canAllocate(requirements.childIndices.count)) ||
      (requirements.brickNodes.count != 0u && !allocator(SparseVoxelPhysicalBufferKind::BrickNodes).canAllocate(requirements.brickNodes.count)) ||
      (requirements.brickOccupancy.count != 0u && !allocator(SparseVoxelPhysicalBufferKind::BrickOccupancy).canAllocate(requirements.brickOccupancy.count)) ||
      (requirements.materials.count != 0u && !allocator(SparseVoxelPhysicalBufferKind::Materials).canAllocate(requirements.materials.count)) ||
      (requirements.materialCells.count != 0u && !allocator(SparseVoxelPhysicalBufferKind::MaterialCells).canAllocate(requirements.materialCells.count)) ||
      (requirements.tree64Nodes.count != 0u && !allocator(SparseVoxelPhysicalBufferKind::Tree64Nodes).canAllocate(requirements.tree64Nodes.count)) ||
      (requirements.tree64Materials.count != 0u && !allocator(SparseVoxelPhysicalBufferKind::Tree64Materials).canAllocate(requirements.tree64Materials.count)) ||
      (requirements.traceNodes.count != 0u && !allocator(SparseVoxelPhysicalBufferKind::TraceNodes).canAllocate(requirements.traceNodes.count)) ||
      (requirements.traceErrorWords.count != 0u && !allocator(SparseVoxelPhysicalBufferKind::TraceErrorWords).canAllocate(requirements.traceErrorWords.count)) ||
      (requirements.cullErrorWords.count != 0u && !allocator(SparseVoxelPhysicalBufferKind::CullErrorWords).canAllocate(requirements.cullErrorWords.count)) ||
      (requirements.triangleClusters.count != 0u && !allocator(SparseVoxelPhysicalBufferKind::TriangleClusters).canAllocate(requirements.triangleClusters.count)) ||
      (requirements.triangleVertices.count != 0u && !allocator(SparseVoxelPhysicalBufferKind::TriangleVertices).canAllocate(requirements.triangleVertices.count)) ||
      (requirements.triangleIndices.count != 0u && !allocator(SparseVoxelPhysicalBufferKind::TriangleIndices).canAllocate(requirements.triangleIndices.count)) ||
      (requirements.virtualGeometryMeshlets.count != 0u && !allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryMeshlets).canAllocate(requirements.virtualGeometryMeshlets.count)) ||
      (requirements.virtualGeometryHierarchyNodes.count != 0u && !allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryHierarchyNodes).canAllocate(requirements.virtualGeometryHierarchyNodes.count)) ||
      (requirements.virtualGeometryGroups.count != 0u && !allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryGroups).canAllocate(requirements.virtualGeometryGroups.count)) ||
      (requirements.virtualGeometryPositionWords.count != 0u && !allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryPositionWords).canAllocate(requirements.virtualGeometryPositionWords.count)) ||
      (requirements.virtualGeometryNormalWords.count != 0u && !allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryNormalWords).canAllocate(requirements.virtualGeometryNormalWords.count)) ||
      (requirements.virtualGeometryUVWords.count != 0u && !allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryUVWords).canAllocate(requirements.virtualGeometryUVWords.count)) ||
      (requirements.virtualGeometryIndexBytes.count != 0u && !allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryIndexBytes).canAllocate(requirements.virtualGeometryIndexBytes.count)) ||
      (requirements.virtualGeometryBoneWeightWords.count != 0u && !allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryBoneWeightWords).canAllocate(requirements.virtualGeometryBoneWeightWords.count)))
  {
    return fail(error, "sparse voxel shared page allocation requirements exceed free physical ranges");
  }

  allocation.page = page;
  allocation.allocationRevision = nextAllocationRevision_++;
  allocation.geometryRevision = requirements.geometryRevision;
  allocation.materialRevision = requirements.materialRevision != 0u || requirements.geometryRevision == 0u
      ? requirements.materialRevision
      : requirements.geometryRevision;
  allocation.traceFormat = requirements.traceFormat;
  allocator(SparseVoxelPhysicalBufferKind::HierarchyNodes).allocate(requirements.hierarchyNodes.count, allocation.hierarchyNodes);
  allocator(SparseVoxelPhysicalBufferKind::ChildIndices).allocate(requirements.childIndices.count, allocation.childIndices);
  allocator(SparseVoxelPhysicalBufferKind::BrickNodes).allocate(requirements.brickNodes.count, allocation.brickNodes);
  allocator(SparseVoxelPhysicalBufferKind::BrickOccupancy).allocate(requirements.brickOccupancy.count, allocation.brickOccupancy);
  allocator(SparseVoxelPhysicalBufferKind::Materials).allocate(requirements.materials.count, allocation.materials);
  allocator(SparseVoxelPhysicalBufferKind::MaterialCells).allocate(requirements.materialCells.count, allocation.materialCells);
  allocator(SparseVoxelPhysicalBufferKind::Tree64Nodes).allocate(requirements.tree64Nodes.count, allocation.tree64Nodes);
  allocator(SparseVoxelPhysicalBufferKind::Tree64Materials).allocate(requirements.tree64Materials.count, allocation.tree64Materials);
  allocator(SparseVoxelPhysicalBufferKind::TraceNodes).allocate(requirements.traceNodes.count, allocation.traceNodes);
  allocator(SparseVoxelPhysicalBufferKind::TraceErrorWords).allocate(requirements.traceErrorWords.count, allocation.traceErrorWords);
  allocator(SparseVoxelPhysicalBufferKind::CullErrorWords).allocate(requirements.cullErrorWords.count, allocation.cullErrorWords);
  allocator(SparseVoxelPhysicalBufferKind::TriangleClusters).allocate(requirements.triangleClusters.count, allocation.triangleClusters);
  allocator(SparseVoxelPhysicalBufferKind::TriangleVertices).allocate(requirements.triangleVertices.count, allocation.triangleVertices);
  allocator(SparseVoxelPhysicalBufferKind::TriangleIndices).allocate(requirements.triangleIndices.count, allocation.triangleIndices);
  allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryMeshlets).allocate(requirements.virtualGeometryMeshlets.count, allocation.virtualGeometryMeshlets);
  allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryHierarchyNodes).allocate(requirements.virtualGeometryHierarchyNodes.count, allocation.virtualGeometryHierarchyNodes);
  allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryGroups).allocate(requirements.virtualGeometryGroups.count, allocation.virtualGeometryGroups);
  allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryPositionWords).allocate(requirements.virtualGeometryPositionWords.count, allocation.virtualGeometryPositionWords);
  allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryNormalWords).allocate(requirements.virtualGeometryNormalWords.count, allocation.virtualGeometryNormalWords);
  allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryUVWords).allocate(requirements.virtualGeometryUVWords.count, allocation.virtualGeometryUVWords);
  allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryIndexBytes).allocate(requirements.virtualGeometryIndexBytes.count, allocation.virtualGeometryIndexBytes);
  allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryBoneWeightWords).allocate(requirements.virtualGeometryBoneWeightWords.count, allocation.virtualGeometryBoneWeightWords);
  if (!assignPackedPageSlot(allocation, payloadBytes, error))
    return false;
  record->allocation = allocation;
  record->state = SparseVoxelPageState::UploadQueued;
  if (error != nullptr)
    error->clear();
  return true;
}

bool SparseVoxelSharedPageTable::validateAllocationActive(const SparseVoxelPhysicalPageAllocation &allocation, std::string *error) const
{
  if (!allocation.valid() || !allocation.hasRenderablePayload())
    return fail(error, "sparse voxel shared page allocation is invalid");
  if (!allocator(SparseVoxelPhysicalBufferKind::HierarchyNodes).containsActive(allocation.hierarchyNodes) || !allocator(SparseVoxelPhysicalBufferKind::ChildIndices).containsActive(allocation.childIndices) ||
      !allocator(SparseVoxelPhysicalBufferKind::BrickNodes).containsActive(allocation.brickNodes) || !allocator(SparseVoxelPhysicalBufferKind::BrickOccupancy).containsActive(allocation.brickOccupancy) ||
      !allocator(SparseVoxelPhysicalBufferKind::Materials).containsActive(allocation.materials) || !allocator(SparseVoxelPhysicalBufferKind::MaterialCells).containsActive(allocation.materialCells) ||
      !allocator(SparseVoxelPhysicalBufferKind::Tree64Nodes).containsActive(allocation.tree64Nodes) || !allocator(SparseVoxelPhysicalBufferKind::Tree64Materials).containsActive(allocation.tree64Materials) ||
      !allocator(SparseVoxelPhysicalBufferKind::TraceNodes).containsActive(allocation.traceNodes) || !allocator(SparseVoxelPhysicalBufferKind::TraceErrorWords).containsActive(allocation.traceErrorWords) ||
      !allocator(SparseVoxelPhysicalBufferKind::CullErrorWords).containsActive(allocation.cullErrorWords) ||
      !allocator(SparseVoxelPhysicalBufferKind::TriangleClusters).containsActive(allocation.triangleClusters) ||
      !allocator(SparseVoxelPhysicalBufferKind::TriangleVertices).containsActive(allocation.triangleVertices) ||
      !allocator(SparseVoxelPhysicalBufferKind::TriangleIndices).containsActive(allocation.triangleIndices) ||
      !allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryMeshlets).containsActive(allocation.virtualGeometryMeshlets) ||
      !allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryHierarchyNodes).containsActive(allocation.virtualGeometryHierarchyNodes) ||
      !allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryGroups).containsActive(allocation.virtualGeometryGroups) ||
      !allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryPositionWords).containsActive(allocation.virtualGeometryPositionWords) ||
      !allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryNormalWords).containsActive(allocation.virtualGeometryNormalWords) ||
      !allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryUVWords).containsActive(allocation.virtualGeometryUVWords) ||
      !allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryIndexBytes).containsActive(allocation.virtualGeometryIndexBytes) ||
      !allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryBoneWeightWords).containsActive(allocation.virtualGeometryBoneWeightWords))
  {
    return fail(error, "sparse voxel shared page allocation is not active");
  }
  if (error != nullptr)
    error->clear();
  return true;
}

bool SparseVoxelSharedPageTable::validateRewriteActive(const SparseVoxelPhysicalPageRewriteGPU &rewrite, std::string *error) const
{
  if (rewrite.nodeIndex == kInvalidSparseVoxelIndex || !allocator(SparseVoxelPhysicalBufferKind::HierarchyNodes).containsActive({rewrite.nodeIndex, 1u}))
  {
    return fail(error, "sparse voxel shared page rewrite targets an unallocated hierarchy node");
  }
  if (rewrite.childCount != 0u && !allocator(SparseVoxelPhysicalBufferKind::ChildIndices).containsActive({rewrite.firstChild, rewrite.childCount}))
  {
    return fail(error, "sparse voxel shared page rewrite targets unallocated child indices");
  }
  if (error != nullptr)
    error->clear();
  return true;
}

bool SparseVoxelSharedPageTable::installPage(
    const SparseVoxelPageId page,
    const SparseVoxelPhysicalPageAllocation &allocation,
    const std::vector<SparseVoxelPageInstallRewrite> &rewrites,
    const uint64_t generation,
    std::string *error)
{
  auto *record = mutableEntry(page);
  if (record == nullptr)
    return fail(error, "sparse voxel shared page is missing");
  if (record->resident() || record->retiring())
    return fail(error, "sparse voxel shared page cannot be installed in its current state");
  if (record->allocation.allocationRevision != allocation.allocationRevision || !(record->allocation.page == allocation.page))
    return fail(error, "sparse voxel shared page install allocation was not reserved for the page");
  if (!(allocation.page == page) || !validateAllocationActive(allocation, error))
    return false;
  const auto *dependencies = dependenciesFor(page);
  if (dependencies == nullptr)
    return fail(error, "sparse voxel shared page dependency directory is missing");
  for (const auto dependency : *dependencies)
  {
    const auto *dependencyEntry = entry(dependency);
    if (dependencyEntry == nullptr || !dependencyEntry->resident())
      return fail(error, "sparse voxel shared page dependency is not resident");
  }
  for (const auto &rewrite : rewrites)
  {
    if (!(rewrite.page == page) || !(rewrite.allocation.page == page) || (rewrite.requiredProducerRevision != 0u && rewrite.requiredProducerRevision != record->producer.producerRevision) ||
        !validateRewriteActive(rewrite.rewrite, error))
    {
      return false;
    }
  }

  record->allocation = allocation;
  record->generation = generation;
  record->geometryRevision = allocation.geometryRevision != 0u ? allocation.geometryRevision : generation;
  record->materialRevision = allocation.materialRevision;
  record->traceFormat = allocation.traceFormat;
  record->state = SparseVoxelPageState::Resident;
  record->retirementFence = 0u;
  markPageTableDirty(static_cast<uint32_t>(record - entries_.data()));
  for (const auto dependency : *dependencies)
    mutableEntry(dependency)->residentDependentCount += 1u;
  const auto *dependents = dependentsFor(page);
  if (dependents != nullptr)
    for (const auto dependent : *dependents)
      mutableEntry(dependent)->residentDependencyCount += 1u;
  installRewrites_.insert(installRewrites_.end(), rewrites.begin(), rewrites.end());
  if (error != nullptr)
    error->clear();
  return true;
}

bool SparseVoxelSharedPageTable::retireAllocation(const SparseVoxelPhysicalPageAllocation &allocation, const uint64_t retirementFence, std::string *error)
{
  if (!allocator(SparseVoxelPhysicalBufferKind::HierarchyNodes).retire(allocation.hierarchyNodes, retirementFence, error) ||
      !allocator(SparseVoxelPhysicalBufferKind::ChildIndices).retire(allocation.childIndices, retirementFence, error) ||
      !allocator(SparseVoxelPhysicalBufferKind::BrickNodes).retire(allocation.brickNodes, retirementFence, error) ||
      !allocator(SparseVoxelPhysicalBufferKind::BrickOccupancy).retire(allocation.brickOccupancy, retirementFence, error) ||
      !allocator(SparseVoxelPhysicalBufferKind::Materials).retire(allocation.materials, retirementFence, error) ||
      !allocator(SparseVoxelPhysicalBufferKind::MaterialCells).retire(allocation.materialCells, retirementFence, error) ||
      !allocator(SparseVoxelPhysicalBufferKind::Tree64Nodes).retire(allocation.tree64Nodes, retirementFence, error) ||
      !allocator(SparseVoxelPhysicalBufferKind::Tree64Materials).retire(allocation.tree64Materials, retirementFence, error) ||
      !allocator(SparseVoxelPhysicalBufferKind::TraceNodes).retire(allocation.traceNodes, retirementFence, error) ||
      !allocator(SparseVoxelPhysicalBufferKind::TraceErrorWords).retire(allocation.traceErrorWords, retirementFence, error) ||
      !allocator(SparseVoxelPhysicalBufferKind::CullErrorWords).retire(allocation.cullErrorWords, retirementFence, error) ||
      !allocator(SparseVoxelPhysicalBufferKind::TriangleClusters).retire(allocation.triangleClusters, retirementFence, error) ||
      !allocator(SparseVoxelPhysicalBufferKind::TriangleVertices).retire(allocation.triangleVertices, retirementFence, error) ||
      !allocator(SparseVoxelPhysicalBufferKind::TriangleIndices).retire(allocation.triangleIndices, retirementFence, error) ||
      !allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryMeshlets).retire(allocation.virtualGeometryMeshlets, retirementFence, error) ||
      !allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryHierarchyNodes).retire(allocation.virtualGeometryHierarchyNodes, retirementFence, error) ||
      !allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryGroups).retire(allocation.virtualGeometryGroups, retirementFence, error) ||
      !allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryPositionWords).retire(allocation.virtualGeometryPositionWords, retirementFence, error) ||
      !allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryNormalWords).retire(allocation.virtualGeometryNormalWords, retirementFence, error) ||
      !allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryUVWords).retire(allocation.virtualGeometryUVWords, retirementFence, error) ||
      !allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryIndexBytes).retire(allocation.virtualGeometryIndexBytes, retirementFence, error) ||
      !allocator(SparseVoxelPhysicalBufferKind::VirtualGeometryBoneWeightWords).retire(allocation.virtualGeometryBoneWeightWords, retirementFence, error))
  {
    return false;
  }
  if (error != nullptr)
    error->clear();
  return true;
}

SparseVoxelPageEvictionDecision SparseVoxelSharedPageTable::evictionDecision(const SparseVoxelPageId page) const
{
  SparseVoxelPageEvictionDecision decision{};
  decision.page = page;

  const auto *record = entry(page);
  if (record == nullptr)
    return decision;

  decision.pageKnown = true;
  decision.state = record->state;
  decision.resident = record->resident();
  decision.pinned = record->pinned;
  decision.retiring = record->retiring();
  decision.hasLiveAllocation = record->allocation.hasRenderablePayload();
  decision.dependencyCount = record->dependencyCount;
  decision.residentDependencyCount = record->residentDependencyCount;
  decision.residentDependentCount = record->residentDependentCount;

  const auto *dependents = dependentsFor(page);
  if (dependents != nullptr)
  {
    for (const auto dependent : *dependents)
    {
      const auto *dependentEntry = entry(dependent);
      if (dependentEntry != nullptr && isSparseVoxelPageLiveOrInFlight(dependentEntry->state))
        ++decision.liveDependentCount;
    }
  }

  decision.evictable = decision.resident && !decision.pinned && decision.liveDependentCount == 0u;
  return decision;
}

bool SparseVoxelSharedPageTable::canEvictPage(const SparseVoxelPageId page, SparseVoxelPageEvictionDecision *outDecision, std::string *error) const
{
  const auto decision = evictionDecision(page);
  if (outDecision != nullptr)
    *outDecision = decision;
  if (!decision.pageKnown)
    return fail(error, "sparse voxel shared page is missing");
  if (!decision.resident)
    return fail(error, "sparse voxel shared page is not resident");
  if (decision.pinned)
    return fail(error, "sparse voxel shared page is pinned");
  if (decision.liveDependentCount != 0u)
    return fail(error, "sparse voxel shared page still has live dependent pages");
  if (error != nullptr)
    error->clear();
  return true;
}

SparseVoxelPageConsumerReferenceSummary summarizeSparseVoxelPageConsumerReferences(
    const SparseVoxelSharedPageTable &table,
    const SparseVoxelPageId page,
    const std::vector<SparseVoxelPageConsumerReference> &references)
{
  SparseVoxelPageConsumerReferenceSummary summary{};
  summary.page = page;
  const auto eviction = table.evictionDecision(page);
  summary.pageKnown = eviction.pageKnown;
  summary.resident = eviction.resident;
  summary.baseEvictable = eviction.evictable;
  const auto *entry = table.entry(page);
  for (const auto &reference : references)
  {
    if (!(reference.page == page))
      continue;
    if (entry == nullptr || !entry->resident() ||
        (reference.generation != 0u && reference.generation != entry->generation))
    {
      ++summary.staleConsumerCount;
      continue;
    }
    if (reference.kind == SparseVoxelPageConsumerKind::Raster)
      ++summary.rasterConsumerCount;
    else
      ++summary.traceConsumerCount;
  }
  summary.evictable =
      summary.baseEvictable &&
      summary.rasterConsumerCount == 0u &&
      summary.traceConsumerCount == 0u;
  return summary;
}

bool canEvictSparseVoxelPageWithConsumers(
    const SparseVoxelSharedPageTable &table,
    const SparseVoxelPageId page,
    const std::vector<SparseVoxelPageConsumerReference> &references,
    SparseVoxelPageConsumerReferenceSummary *outSummary,
    std::string *error)
{
  const auto summary =
      summarizeSparseVoxelPageConsumerReferences(table, page, references);
  if (outSummary != nullptr)
    *outSummary = summary;
  if (!summary.pageKnown)
  {
    if (error != nullptr)
      *error = "sparse voxel consumer-protected page is missing";
    return false;
  }
  if (!summary.resident)
  {
    if (error != nullptr)
      *error = "sparse voxel consumer-protected page is not resident";
    return false;
  }
  if (!summary.baseEvictable)
  {
    if (error != nullptr)
      *error = "sparse voxel consumer-protected page is blocked by residency dependencies or pinning";
    return false;
  }
  if (summary.rasterConsumerCount != 0u || summary.traceConsumerCount != 0u)
  {
    if (error != nullptr)
      *error = "sparse voxel consumer-protected page still has raster or trace consumers";
    return false;
  }
  if (error != nullptr)
    error->clear();
  return true;
}

bool SparseVoxelSharedPageTable::uninstallPage(const SparseVoxelPageId page, const std::vector<SparseVoxelPageUninstallRewrite> &rewrites, const uint64_t retirementFence, std::string *error)
{
  auto *record = mutableEntry(page);
  if (record == nullptr)
    return fail(error, "sparse voxel shared page is missing");
  if (!canEvictPage(page, nullptr, error))
    return false;
  if (retirementFence < completedFence_)
    return fail(error, "sparse voxel shared page retirement fence is stale");
  if (!validateAllocationActive(record->allocation, error))
    return false;
  for (const auto &rewrite : rewrites)
  {
    if (!(rewrite.page == page) || (rewrite.retiredAllocationRevision != 0u && rewrite.retiredAllocationRevision != record->allocation.allocationRevision) || !validateRewriteActive(rewrite.rewrite, error))
    {
      return false;
    }
  }
  if (!retireAllocation(record->allocation, retirementFence, error))
    return false;

  const auto *dependencies = dependenciesFor(page);
  if (dependencies != nullptr)
    for (const auto dependency : *dependencies)
      mutableEntry(dependency)->residentDependentCount -= 1u;
  const auto *dependents = dependentsFor(page);
  if (dependents != nullptr)
    for (const auto dependent : *dependents)
    {
      auto *dependentEntry = mutableEntry(dependent);
      if (dependentEntry->residentDependencyCount != 0u)
        dependentEntry->residentDependencyCount -= 1u;
    }

  record->state = SparseVoxelPageState::EvictPending;
  record->retirementFence = retirementFence;
  markPageTableDirty(static_cast<uint32_t>(record - entries_.data()));
  uninstallRewrites_.insert(uninstallRewrites_.end(), rewrites.begin(), rewrites.end());
  if (error != nullptr)
    error->clear();
  return true;
}

bool SparseVoxelSharedPageTable::reserveProducedPagePayload(const SparseVoxelProducedPagePayload &payload, SparseVoxelReservedPagePayload &reservation, std::string *error)
{
  reservation = {};
  if (!payload.valid())
    return fail(error, "sparse voxel produced page payload metadata is invalid");
  auto *record = mutableEntry(payload.page);
  if (record == nullptr)
    return fail(error, "sparse voxel produced page target is missing");
  if (payload.expectedPageGeneration != record->generation)
    return fail(error, "sparse voxel produced page generation is stale");
  if (payload.requiredProducerRevision != 0u && payload.requiredProducerRevision != record->producer.producerRevision)
  {
    return fail(error, "sparse voxel produced page producer revision is stale");
  }
  const bool triangleRasterOnly =
      payload.requirements.requiresTriangleRasterPayload() &&
      payload.requirements.hierarchyNodes.count == 0u &&
      payload.requirements.childIndices.count == 0u &&
      payload.requirements.brickNodes.count == 0u &&
      payload.requirements.brickOccupancy.count == 0u &&
      payload.requirements.materials.count == 0u &&
      payload.requirements.materialCells.count == 0u &&
      payload.requirements.tree64Nodes.count == 0u &&
      payload.requirements.tree64Materials.count == 0u &&
      payload.requirements.traceNodes.count == 0u &&
      payload.requirements.traceErrorWords.count == 0u &&
      payload.requirements.cullErrorWords.count == 0u;
  if (payload.installRewriteCount == 0u && !triangleRasterOnly)
    return fail(error, "sparse voxel produced page requires install rewrites");
  const bool tracePayload = payload.requirements.traceNodes.count != 0u || payload.requirements.traceErrorWords.count != 0u;
  if (tracePayload)
  {
    if (payload.geometryRevision == 0u || payload.requirements.geometryRevision != payload.geometryRevision)
      return fail(error, "sparse voxel produced page trace view geometry revision mismatch");
  }
  if (payload.requirements.cullErrorWords.count != 0u && payload.geometryRevision == 0u)
    return fail(error, "sparse voxel produced page cull sidecar geometry revision is invalid");
  if (payload.materialRevision != 0u && payload.geometryRevision != 0u && payload.materialRevision != payload.geometryRevision)
    return fail(error, "sparse voxel produced page material revision does not match geometry revision");
  if (payload.materialRevision != 0u && payload.requirements.materialRevision != 0u && payload.requirements.materialRevision != payload.materialRevision)
    return fail(error, "sparse voxel produced page material view revision mismatch");

  SparseVoxelPhysicalPageAllocation allocation{};
  if (!allocatePage(payload.page, payload.requirements, allocation, error))
    return false;
  reservation.produced = payload;
  reservation.allocation = allocation;
  if (error != nullptr)
    error->clear();
  return true;
}

bool SparseVoxelSharedPageTable::markPayloadCopied(SparseVoxelReservedPagePayload &reservation, const uint64_t copyFence, std::string *error) const
{
  if (!reservation.valid())
    return fail(error, "sparse voxel reserved page payload is invalid");
  const auto *record = entry(reservation.produced.page);
  if (record == nullptr)
    return fail(error, "sparse voxel reserved page target is missing");
  if (record->resident() || record->retiring())
    return fail(error, "sparse voxel reserved page is already render-visible or retiring");
  if (record->generation != reservation.produced.expectedPageGeneration)
    return fail(error, "sparse voxel reserved page generation is stale");
  if (record->allocation.allocationRevision != reservation.allocation.allocationRevision || !(record->allocation.page == reservation.allocation.page))
  {
    return fail(error, "sparse voxel reserved page allocation is not current");
  }
  if (!validateAllocationActive(reservation.allocation, error))
    return false;
  reservation.copyFence = copyFence;
  reservation.copied = true;
  if (error != nullptr)
    error->clear();
  return true;
}

bool SparseVoxelSharedPageTable::installCopiedPagePayload(const SparseVoxelReservedPagePayload &reservation, const std::vector<SparseVoxelPageInstallRewrite> &rewrites, std::string *error)
{
  if (!reservation.valid() || !reservation.copied)
    return fail(error, "sparse voxel copied page payload is not ready for install");
  const auto *record = entry(reservation.produced.page);
  if (record == nullptr)
    return fail(error, "sparse voxel copied page target is missing");
  if (record->generation != reservation.produced.expectedPageGeneration)
    return fail(error, "sparse voxel copied page generation is stale");
  if (record->producer.producerRevision != reservation.produced.requiredProducerRevision)
    return fail(error, "sparse voxel copied page producer revision is stale");
  if (rewrites.size() != reservation.produced.installRewriteCount)
    return fail(error, "sparse voxel copied page install rewrite count mismatch");
  return installPage(reservation.produced.page, reservation.allocation, rewrites, reservation.produced.payloadRevision, error);
}

uint32_t SparseVoxelSharedPageTable::reclaim(const uint64_t completedFence)
{
  completedFence_ = std::max(completedFence_, completedFence);
  uint32_t reclaimed = 0u;
  for (uint32_t index = 0u; index < static_cast<uint32_t>(SparseVoxelPhysicalBufferKind::Count); ++index)
    reclaimed += allocators_[index].reclaim(completedFence_);
  for (uint32_t index = 0u; index < entries_.size(); ++index)
  {
    auto &record = entries_[index];
    if (record.retiring() && record.retirementFence <= completedFence_)
    {
      record.state = SparseVoxelPageState::Unloaded;
      record.allocation = {};
      record.retirementFence = 0u;
      record.geometryRevision = 0u;
      record.materialRevision = 0u;
      record.traceFormat = kVoxelTraceFormatInvalid;
      ++record.generation;
      markPageTableDirty(index);
    }
  }
  return reclaimed;
}

std::vector<SparseVoxelSharedPageTableEntry> SparseVoxelSharedPageTable::debugReadback() const
{
  return entries_;
}

std::vector<SparseVoxelPackedPageSlotDebug> SparseVoxelSharedPageTable::packedPageSlots() const
{
  std::vector<SparseVoxelPackedPageSlotDebug> result;
  result.reserve(packedPageSlots_.size());
  for (uint32_t slot = 0u; slot < packedPageSlots_.size(); ++slot)
  {
    const auto decision = packedPageSlotDecision(slot);
    result.push_back(
        {
          slot,
          packedPageSlotBytes_,
          decision.usedBytes,
          decision.pageletCount,
          decision.livePageletCount,
          decision.residentPageletCount,
          decision.retiringPageletCount,
        });
  }
  return result;
}

SparseVoxelPhysicalAllocatorStats SparseVoxelSharedPageTable::allocatorStats(const SparseVoxelPhysicalBufferKind kind) const
{
  return allocator(kind).stats();
}

bool SparseVoxelPageResidency::fail(std::string *error, const std::string &message) const
{
  if (error != nullptr)
    *error = message;
  return false;
}

bool SparseVoxelPageResidency::validPage(const uint32_t pageIndex, std::string *error) const
{
  if (!initialized_)
    return fail(error, "sparse voxel residency is not initialized");
  if (pageIndex >= pageStates_.size())
    return fail(error, "sparse voxel page index is out of range");
  return true;
}

bool SparseVoxelPageResidency::initialize(const VirtualSparseVoxelSceneMetadata &metadata, const uint32_t physicalNodeCapacity, std::string *error)
{
  if (metadata.pageCount == 0u || metadata.pages.size() != metadata.pageCount || metadata.nodeCount == 0u)
    return fail(error, "sparse voxel residency requires non-empty validated metadata");
  if (physicalNodeCapacity == 0u)
    return fail(error, "sparse voxel physical node capacity must be non-zero");

  metadata_ = metadata;
  initialized_ = true;
  ++generation_;
  contentRevision_ = 0u;
  physicalNodeCapacity_ = physicalNodeCapacity;
  residentNodeCount_ = 0u;
  pendingNodeCount_ = 0u;
  diagnostics_ = {};
  pageStates_.assign(metadata.pageCount, {});
  pageContentRevisions_.assign(metadata.pageCount, 0u);
  dependents_.assign(metadata.pageCount, {});
  refinementPages_.assign(metadata.nodeCount, kInvalidSparseVoxelIndex);
  logicalToPhysical_.assign(metadata.nodeCount, kInvalidSparseVoxelIndex);
  freeRanges_ = {{0u, physicalNodeCapacity}};
  physicalNodes_.assign(physicalNodeCapacity, {});
  physicalPathsLevels_.assign(physicalNodeCapacity, {});
  physicalCentersLengths_.assign(physicalNodeCapacity, {});
  physicalMaterials_.assign(physicalNodeCapacity, {});
  physicalMetadata_.assign(physicalNodeCapacity, {});

  for (uint32_t pageIndex = 0u; pageIndex < metadata.pageCount; ++pageIndex)
  {
    const auto &page = metadata.pages[pageIndex];
    pageStates_[pageIndex].pinned = (page.flags & kVirtualSparseVoxelPageRoot) != 0u;
    if (page.dependencyFirst > metadata.pageDependencies.size() || page.dependencyCount > metadata.pageDependencies.size() - page.dependencyFirst)
      return fail(error, "sparse voxel page dependency range is invalid");
    for (uint32_t dependencyOffset = 0u; dependencyOffset < page.dependencyCount; ++dependencyOffset)
    {
      const uint32_t dependency = metadata.pageDependencies[static_cast<size_t>(page.dependencyFirst) + dependencyOffset];
      if (dependency >= metadata.pageCount)
        return fail(error, "sparse voxel page dependency is out of range");
      dependents_[dependency].push_back(pageIndex);
    }
    if (page.installFirst > metadata.installUpdates.size() || page.installCount > metadata.installUpdates.size() - page.installFirst)
      return fail(error, "sparse voxel page install rewrite range is invalid");
    for (uint32_t updateOffset = 0u; updateOffset < page.installCount; ++updateOffset)
    {
      const auto &rewrite = metadata.installUpdates[static_cast<size_t>(page.installFirst) + updateOffset];
      if (rewrite.nodeIndex >= metadata.nodeCount)
        return fail(error, "sparse voxel install rewrite node is out of range");
      uint32_t &refinementPage = refinementPages_[rewrite.nodeIndex];
      if (refinementPage != kInvalidSparseVoxelIndex && refinementPage != pageIndex)
        return fail(error, "sparse voxel node has more than one refinement page");
      refinementPage = pageIndex;
    }
  }

  for (auto &pageDependents : dependents_)
    std::sort(pageDependents.begin(), pageDependents.end());

  // Roots are always requested and pinned. They still travel through the same
  // read/decode/upload state machine as every other page.
  for (uint32_t pageIndex = 0u; pageIndex < metadata.pageCount; ++pageIndex)
  {
    if (pageStates_[pageIndex].pinned)
    {
      pageStates_[pageIndex].state = SparseVoxelPageState::Requested;
      pageStates_[pageIndex].priority = std::numeric_limits<uint32_t>::max();
    }
  }
  if (error != nullptr)
    error->clear();
  return true;
}

bool SparseVoxelPageResidency::requestPage(const uint32_t pageIndex, const uint32_t priority, const uint32_t instanceIndex, const uint64_t frameIndex, std::string *error)
{
  if (!validPage(pageIndex, error))
    return false;
  auto &state = pageStates_[pageIndex];
  if (state.state == SparseVoxelPageState::Unloaded)
    state.state = SparseVoxelPageState::Requested;
  if (priority > state.priority || (priority == state.priority && instanceIndex < state.requestingInstance))
  {
    state.priority = priority;
    state.requestingInstance = instanceIndex;
  }
  state.lastRequestedFrame = std::max(state.lastRequestedFrame, frameIndex);
  if (error != nullptr)
    error->clear();
  return true;
}

std::vector<uint32_t> SparseVoxelPageResidency::planAdmissions(const SparseVoxelStreamingBudgets &budgets)
{
  std::vector<uint32_t> result;
  if (!initialized_ || budgets.maxPages == 0u || budgets.maxNodes == 0u || budgets.maxStoredBytes == 0u)
    return result;

  std::vector<bool> needed(pageStates_.size(), false);
  std::vector<uint32_t> inheritedPriority(pageStates_.size(), 0u);
  std::vector<uint32_t> inheritedInstance(pageStates_.size(), kInvalidSparseVoxelIndex);
  std::vector<uint32_t> stack;
  for (uint32_t pageIndex = 0u; pageIndex < pageStates_.size(); ++pageIndex)
  {
    const auto state = pageStates_[pageIndex].state;
    if (state == SparseVoxelPageState::Requested)
    {
      stack.push_back(pageIndex);
      inheritedPriority[pageIndex] = pageStates_[pageIndex].priority;
      inheritedInstance[pageIndex] = pageStates_[pageIndex].requestingInstance;
    }
  }

  while (!stack.empty())
  {
    const uint32_t pageIndex = stack.back();
    stack.pop_back();
    needed[pageIndex] = true;
    const auto &page = metadata_.pages[pageIndex];
    for (uint32_t dependencyOffset = 0u; dependencyOffset < page.dependencyCount; ++dependencyOffset)
    {
      const uint32_t dependency = metadata_.pageDependencies[static_cast<size_t>(page.dependencyFirst) + dependencyOffset];
      const bool stronger = inheritedPriority[pageIndex] > inheritedPriority[dependency] || (inheritedPriority[pageIndex] == inheritedPriority[dependency] && inheritedInstance[pageIndex] < inheritedInstance[dependency]);
      if (stronger)
      {
        inheritedPriority[dependency] = inheritedPriority[pageIndex];
        inheritedInstance[dependency] = inheritedInstance[pageIndex];
      }
      if (!needed[dependency])
        stack.push_back(dependency);
    }
  }

  for (uint32_t pageIndex = 0u; pageIndex < pageStates_.size(); ++pageIndex)
  {
    if (needed[pageIndex] && pageStates_[pageIndex].state == SparseVoxelPageState::Unloaded)
    {
      pageStates_[pageIndex].state = SparseVoxelPageState::Requested;
      pageStates_[pageIndex].priority = inheritedPriority[pageIndex];
      pageStates_[pageIndex].requestingInstance = inheritedInstance[pageIndex];
    }
  }

  std::vector<bool> available(pageStates_.size(), false);
  for (uint32_t pageIndex = 0u; pageIndex < pageStates_.size(); ++pageIndex)
    available[pageIndex] = pageStates_[pageIndex].state == SparseVoxelPageState::Resident;

  uint64_t storedBytes = 0u;
  uint32_t nodes = 0u;
  const uint32_t admittedNodeCapacity = std::min(budgets.maxNodes, freeNodeCount());
  while (result.size() < budgets.maxPages)
  {
    uint32_t best = kInvalidSparseVoxelIndex;
    for (uint32_t pageIndex = 0u; pageIndex < pageStates_.size(); ++pageIndex)
    {
      if (!needed[pageIndex] || available[pageIndex] || pageStates_[pageIndex].state != SparseVoxelPageState::Requested)
        continue;
      const auto &page = metadata_.pages[pageIndex];
      bool ready = true;
      for (uint32_t dependencyOffset = 0u; dependencyOffset < page.dependencyCount; ++dependencyOffset)
      {
        const uint32_t dependency = metadata_.pageDependencies[static_cast<size_t>(page.dependencyFirst) + dependencyOffset];
        ready = ready && available[dependency];
      }
      if (!ready || page.nodeCount > admittedNodeCapacity - nodes || page.storedSize > budgets.maxStoredBytes - storedBytes)
        continue;
      if (best == kInvalidSparseVoxelIndex || pageStates_[pageIndex].priority > pageStates_[best].priority ||
          (pageStates_[pageIndex].priority == pageStates_[best].priority && (page.minLevel < metadata_.pages[best].minLevel || (page.minLevel == metadata_.pages[best].minLevel && pageIndex < best))))
        best = pageIndex;
    }
    if (best == kInvalidSparseVoxelIndex)
      break;
    result.push_back(best);
    available[best] = true;
    nodes += metadata_.pages[best].nodeCount;
    storedBytes += metadata_.pages[best].storedSize;
  }
  return result;
}

bool SparseVoxelPageResidency::markReading(const uint32_t pageIndex, std::string *error)
{
  if (!validPage(pageIndex, error))
    return false;
  if (pageStates_[pageIndex].state != SparseVoxelPageState::Requested)
    return fail(error, "sparse voxel page must be requested before reading");
  pageStates_[pageIndex].state = SparseVoxelPageState::Reading;
  return true;
}

bool SparseVoxelPageResidency::markDecoded(const uint32_t pageIndex, std::string *error)
{
  if (!validPage(pageIndex, error))
    return false;
  if (pageStates_[pageIndex].state != SparseVoxelPageState::Reading)
    return fail(error, "sparse voxel page must be reading before decode completion");
  pageStates_[pageIndex].state = SparseVoxelPageState::Decoded;
  return true;
}

bool SparseVoxelPageResidency::failIO(const uint32_t pageIndex, const bool retry, std::string *error)
{
  if (!validPage(pageIndex, error))
    return false;
  auto &state = pageStates_[pageIndex];
  if (state.state != SparseVoxelPageState::Reading && state.state != SparseVoxelPageState::Decoded)
    return fail(error, "only a reading or decoded sparse voxel page can fail IO");
  if (retry || state.pinned)
  {
    state.state = SparseVoxelPageState::Requested;
  }
  else
  {
    state.state = SparseVoxelPageState::Unloaded;
    state.priority = 0u;
    state.requestingInstance = kInvalidSparseVoxelIndex;
  }
  if (error != nullptr)
    error->clear();
  return true;
}

bool SparseVoxelPageResidency::dependenciesResident(const uint32_t pageIndex) const
{
  const auto &page = metadata_.pages[pageIndex];
  for (uint32_t dependencyOffset = 0u; dependencyOffset < page.dependencyCount; ++dependencyOffset)
  {
    const uint32_t dependency = metadata_.pageDependencies[static_cast<size_t>(page.dependencyFirst) + dependencyOffset];
    if (pageStates_[dependency].state != SparseVoxelPageState::Resident)
      return false;
  }
  return true;
}

uint32_t SparseVoxelPageResidency::allocate(const uint32_t count)
{
  for (size_t rangeIndex = 0u; rangeIndex < freeRanges_.size(); ++rangeIndex)
  {
    auto &range = freeRanges_[rangeIndex];
    if (range.count < count)
      continue;
    const uint32_t first = range.first;
    range.first += count;
    range.count -= count;
    if (range.count == 0u)
      freeRanges_.erase(freeRanges_.begin() + static_cast<std::ptrdiff_t>(rangeIndex));
    return first;
  }
  return kInvalidSparseVoxelIndex;
}

void SparseVoxelPageResidency::release(const uint32_t first, const uint32_t count)
{
  if (count == 0u)
    return;
  freeRanges_.push_back({first, count});
  std::sort(
      freeRanges_.begin(),
      freeRanges_.end(),
      [](const FreeRange &a, const FreeRange &b)
      {
        return a.first < b.first;
      });
  std::vector<FreeRange> merged;
  for (const FreeRange range : freeRanges_)
  {
    if (!merged.empty() && merged.back().first + merged.back().count == range.first)
      merged.back().count += range.count;
    else
      merged.push_back(range);
  }
  freeRanges_ = std::move(merged);
}

bool SparseVoxelPageResidency::prepareInstall(const VirtualSparseVoxelPageData &page, SparseVoxelPageInstallBatch &batch, std::string *error)
{
  if (!validPage(page.pageIndex, error))
    return false;
  const auto &record = metadata_.pages[page.pageIndex];
  auto &state = pageStates_[page.pageIndex];
  if (state.state != SparseVoxelPageState::Decoded)
    return fail(error, "sparse voxel page must be decoded before upload");
  if (!dependenciesResident(page.pageIndex))
    return fail(error, "sparse voxel page dependencies are not resident");
  if (page.modelIndex != record.modelIndex || page.firstNode != record.firstNode || page.nodes.size() != record.nodeCount || page.pathsLevels.size() != record.nodeCount ||
      page.centersLengths.size() != record.nodeCount || page.materials.size() != record.nodeCount)
    return fail(error, "sparse voxel decoded page does not match metadata");

  const uint32_t physicalFirst = allocate(record.nodeCount);
  if (physicalFirst == kInvalidSparseVoxelIndex)
    return fail(error, "sparse voxel physical node allocation failed");

  batch = {};
  batch.generation = generation_;
  batch.pageIndex = page.pageIndex;
  batch.physicalFirstNode = physicalFirst;
  batch.nodes = page.nodes;
  batch.pathsLevels = page.pathsLevels;
  batch.centersLengths = page.centersLengths;
  batch.materials = page.materials;
  batch.metadata.resize(record.nodeCount);
  for (uint32_t nodeOffset = 0u; nodeOffset < record.nodeCount; ++nodeOffset)
  {
    const uint32_t logicalNode = record.firstNode + nodeOffset;
    batch.nodes[nodeOffset].firstChild = 0u;
    batch.nodes[nodeOffset].childCount = 0u;
    batch.nodes[nodeOffset].voxelIndex = physicalFirst + nodeOffset;
    batch.metadata[nodeOffset] = {logicalNode, refinementPages_[logicalNode], page.pageIndex, 0u};
  }
  batch.publishRewrites.reserve(record.installCount);
  for (uint32_t updateOffset = 0u; updateOffset < record.installCount; ++updateOffset)
  {
    const auto &logical = metadata_.installUpdates[static_cast<size_t>(record.installFirst) + updateOffset];
    const uint32_t physicalParent = logicalToPhysical(logical.nodeIndex);
    if (physicalParent == kInvalidSparseVoxelIndex || logical.firstChild < record.firstNode || logical.firstChild + logical.childCount > record.firstNode + record.nodeCount)
    {
      release(physicalFirst, record.nodeCount);
      return fail(error, "sparse voxel install rewrite cannot be translated to resident physical nodes");
    }
    batch.publishRewrites.push_back({physicalParent, physicalFirst + logical.firstChild - record.firstNode, logical.childCount, logical.flags});
  }
  state.state = SparseVoxelPageState::UploadQueued;
  state.physicalFirstNode = physicalFirst;
  pendingNodeCount_ += record.nodeCount;
  if (error != nullptr)
    error->clear();
  return true;
}

void SparseVoxelPageResidency::applyRewrite(const SparseVoxelPhysicalPageRewriteGPU &rewrite)
{
  physicalNodes_[rewrite.nodeIndex].firstChild = rewrite.firstChild;
  physicalNodes_[rewrite.nodeIndex].childCount = rewrite.childCount;
}

bool SparseVoxelPageResidency::commitInstall(const SparseVoxelPageInstallBatch &batch, std::string *error)
{
  if (batch.generation != generation_ || !validPage(batch.pageIndex, error))
  {
    ++diagnostics_.invalidGenerationFallbackCount;
    return fail(error, "stale sparse voxel install batch");
  }
  auto &state = pageStates_[batch.pageIndex];
  const auto &record = metadata_.pages[batch.pageIndex];
  if (state.state != SparseVoxelPageState::UploadQueued || state.physicalFirstNode != batch.physicalFirstNode || batch.nodes.size() != record.nodeCount || batch.pathsLevels.size() != record.nodeCount ||
      batch.centersLengths.size() != record.nodeCount || batch.materials.size() != record.nodeCount || batch.metadata.size() != record.nodeCount || batch.physicalFirstNode > physicalNodeCapacity_ ||
      record.nodeCount > physicalNodeCapacity_ - batch.physicalFirstNode)
    return fail(error, "sparse voxel install batch does not match queued upload");
  for (const auto &rewrite : batch.publishRewrites)
  {
    if (rewrite.nodeIndex >= physicalNodeCapacity_ || rewrite.firstChild > physicalNodeCapacity_ || rewrite.childCount > physicalNodeCapacity_ - rewrite.firstChild)
      return fail(error, "sparse voxel install batch contains an invalid physical rewrite");
  }

  const uint32_t first = batch.physicalFirstNode;
  std::copy(batch.nodes.begin(), batch.nodes.end(), physicalNodes_.begin() + first);
  std::copy(batch.pathsLevels.begin(), batch.pathsLevels.end(), physicalPathsLevels_.begin() + first);
  std::copy(batch.centersLengths.begin(), batch.centersLengths.end(), physicalCentersLengths_.begin() + first);
  std::copy(batch.materials.begin(), batch.materials.end(), physicalMaterials_.begin() + first);
  std::copy(batch.metadata.begin(), batch.metadata.end(), physicalMetadata_.begin() + first);
  for (uint32_t nodeOffset = 0u; nodeOffset < record.nodeCount; ++nodeOffset)
    logicalToPhysical_[record.firstNode + nodeOffset] = first + nodeOffset;
  for (const auto &rewrite : batch.publishRewrites)
    applyRewrite(rewrite);

  pendingNodeCount_ -= record.nodeCount;
  residentNodeCount_ += record.nodeCount;
  state.state = SparseVoxelPageState::Resident;
  ++diagnostics_.pageInstallCount;
  ++contentRevision_;
  pageContentRevisions_[batch.pageIndex] = contentRevision_;
  if (error != nullptr)
    error->clear();
  return true;
}

bool SparseVoxelPageResidency::cancelInstall(const SparseVoxelPageInstallBatch &batch, std::string *error)
{
  if (batch.generation != generation_ || !validPage(batch.pageIndex, error))
    return fail(error, "stale sparse voxel install batch");
  auto &state = pageStates_[batch.pageIndex];
  const auto &record = metadata_.pages[batch.pageIndex];
  if (state.state != SparseVoxelPageState::UploadQueued || state.physicalFirstNode != batch.physicalFirstNode)
    return fail(error, "sparse voxel install batch is not queued");
  release(batch.physicalFirstNode, record.nodeCount);
  pendingNodeCount_ -= record.nodeCount;
  state.state = SparseVoxelPageState::Decoded;
  state.physicalFirstNode = kInvalidSparseVoxelIndex;
  return true;
}

bool SparseVoxelPageResidency::hasResidentDependent(const uint32_t pageIndex) const
{
  for (const uint32_t dependent : dependents_[pageIndex])
  {
    const auto state = pageStates_[dependent].state;
    if (state == SparseVoxelPageState::Resident || state == SparseVoxelPageState::EvictPending || state == SparseVoxelPageState::UploadQueued)
      return true;
  }
  return false;
}

std::vector<uint32_t> SparseVoxelPageResidency::planEvictions(const uint32_t nodesToFree, const std::vector<uint32_t> &protectedPages) const
{
  std::vector<uint32_t> result;
  if (!initialized_ || nodesToFree == 0u)
    return result;
  std::vector<bool> protectedSet(pageStates_.size(), false);
  std::vector<uint32_t> stack = protectedPages;
  while (!stack.empty())
  {
    const uint32_t pageIndex = stack.back();
    stack.pop_back();
    if (pageIndex >= pageStates_.size() || protectedSet[pageIndex])
      continue;
    protectedSet[pageIndex] = true;
    const auto &page = metadata_.pages[pageIndex];
    for (uint32_t dependencyOffset = 0u; dependencyOffset < page.dependencyCount; ++dependencyOffset)
      stack.push_back(metadata_.pageDependencies[static_cast<size_t>(page.dependencyFirst) + dependencyOffset]);
  }

  std::vector<bool> selected(pageStates_.size(), false);
  uint32_t selectedNodes = 0u;
  while (selectedNodes < nodesToFree)
  {
    uint32_t best = kInvalidSparseVoxelIndex;
    for (uint32_t pageIndex = 0u; pageIndex < pageStates_.size(); ++pageIndex)
    {
      const auto &state = pageStates_[pageIndex];
      if (state.state != SparseVoxelPageState::Resident || state.pinned || protectedSet[pageIndex] || selected[pageIndex])
        continue;
      bool hasDependent = false;
      for (const uint32_t dependent : dependents_[pageIndex])
        hasDependent = hasDependent || (pageStates_[dependent].state == SparseVoxelPageState::Resident && !selected[dependent]);
      if (hasDependent)
        continue;
      if (best == kInvalidSparseVoxelIndex || std::tie(state.priority, state.lastRequestedFrame) < std::tie(pageStates_[best].priority, pageStates_[best].lastRequestedFrame) ||
          (state.priority == pageStates_[best].priority && state.lastRequestedFrame == pageStates_[best].lastRequestedFrame &&
           (metadata_.pages[pageIndex].maxLevel > metadata_.pages[best].maxLevel || (metadata_.pages[pageIndex].maxLevel == metadata_.pages[best].maxLevel && pageIndex > best))))
        best = pageIndex;
    }
    if (best == kInvalidSparseVoxelIndex)
      break;
    selected[best] = true;
    result.push_back(best);
    selectedNodes += metadata_.pages[best].nodeCount;
  }
  return result;
}

bool SparseVoxelPageResidency::prepareEviction(const uint32_t pageIndex, SparseVoxelPageEvictionBatch &batch, std::string *error)
{
  if (!validPage(pageIndex, error))
    return false;
  auto &state = pageStates_[pageIndex];
  const auto &record = metadata_.pages[pageIndex];
  if (state.state != SparseVoxelPageState::Resident)
    return fail(error, "sparse voxel page must be resident before eviction");
  if (state.pinned)
    return fail(error, "sparse voxel root page is pinned");
  if (hasResidentDependent(pageIndex))
    return fail(error, "sparse voxel page must evict resident descendants first");

  batch = {};
  batch.generation = generation_;
  batch.pageIndex = pageIndex;
  batch.physicalFirstNode = state.physicalFirstNode;
  batch.nodeCount = record.nodeCount;
  batch.unpublishRewrites.reserve(record.uninstallCount);
  for (uint32_t updateOffset = 0u; updateOffset < record.uninstallCount; ++updateOffset)
  {
    const auto &logical = metadata_.uninstallUpdates[static_cast<size_t>(record.uninstallFirst) + updateOffset];
    const uint32_t physicalParent = logicalToPhysical(logical.nodeIndex);
    if (physicalParent == kInvalidSparseVoxelIndex)
      return fail(error, "sparse voxel uninstall parent is not resident");
    batch.unpublishRewrites.push_back({physicalParent, 0u, 0u, logical.flags});
  }
  state.state = SparseVoxelPageState::EvictPending;
  return true;
}

bool SparseVoxelPageResidency::commitEviction(const SparseVoxelPageEvictionBatch &batch, std::string *error)
{
  if (batch.generation != generation_ || !validPage(batch.pageIndex, error))
  {
    ++diagnostics_.invalidGenerationFallbackCount;
    return fail(error, "stale sparse voxel eviction batch");
  }
  auto &state = pageStates_[batch.pageIndex];
  const auto &record = metadata_.pages[batch.pageIndex];
  if (state.state != SparseVoxelPageState::EvictPending || state.physicalFirstNode != batch.physicalFirstNode || batch.nodeCount != record.nodeCount || batch.physicalFirstNode > physicalNodeCapacity_ ||
      batch.nodeCount > physicalNodeCapacity_ - batch.physicalFirstNode)
    return fail(error, "sparse voxel eviction batch does not match pending eviction");
  for (const auto &rewrite : batch.unpublishRewrites)
  {
    if (rewrite.nodeIndex >= physicalNodeCapacity_ || rewrite.firstChild != 0u || rewrite.childCount != 0u)
      return fail(error, "sparse voxel eviction batch contains an invalid physical rewrite");
  }
  for (const auto &rewrite : batch.unpublishRewrites)
    applyRewrite(rewrite);
  for (uint32_t nodeOffset = 0u; nodeOffset < record.nodeCount; ++nodeOffset)
  {
    const uint32_t physical = batch.physicalFirstNode + nodeOffset;
    logicalToPhysical_[record.firstNode + nodeOffset] = kInvalidSparseVoxelIndex;
    physicalNodes_[physical] = {};
    physicalPathsLevels_[physical] = {};
    physicalCentersLengths_[physical] = {};
    physicalMaterials_[physical] = {};
    physicalMetadata_[physical] = {};
  }
  release(batch.physicalFirstNode, record.nodeCount);
  residentNodeCount_ -= record.nodeCount;
  state.state = SparseVoxelPageState::Unloaded;
  ++diagnostics_.pageUninstallCount;
  state.physicalFirstNode = kInvalidSparseVoxelIndex;
  state.priority = 0u;
  state.requestingInstance = kInvalidSparseVoxelIndex;
  ++contentRevision_;
  pageContentRevisions_[batch.pageIndex] = contentRevision_;
  if (error != nullptr)
    error->clear();
  return true;
}

bool SparseVoxelPageResidency::diagnoseCutCoverage(const uint32_t modelIndex, SparseVoxelCutDiagnostics &diagnostics, std::string *error) const
{
  diagnostics = diagnostics_;
  if (!initialized_)
    return fail(error, "sparse voxel residency is not initialized");
  if (modelIndex >= metadata_.models.size())
    return fail(error, "sparse voxel cut diagnostics model is out of range");

  const auto &model = metadata_.models[modelIndex];
  std::vector<uint32_t> stack;
  bool valid = true;
  auto recordMissingSelection = [&]()
  {
    ++diagnostics.selectedNeitherParentNorChildCount;
    ++diagnostics.pageResidencyMissCount;
    valid = false;
  };

  for (uint32_t root = 0u; root < model.rootCount; ++root)
  {
    const uint32_t logicalRoot = model.firstRoot + root;
    const uint32_t physicalRoot = logicalToPhysical(logicalRoot);
    if (physicalRoot == kInvalidSparseVoxelIndex || physicalRoot >= physicalNodeCapacity_)
    {
      recordMissingSelection();
      continue;
    }
    stack.push_back(physicalRoot);
  }

  while (!stack.empty())
  {
    const uint32_t physical = stack.back();
    stack.pop_back();
    if (physical >= physicalNodeCapacity_)
    {
      recordMissingSelection();
      continue;
    }

    const auto &node = physicalNodes_[physical];
    const auto &metadata = physicalMetadata_[physical];
    if (metadata.logicalNodeIndex == kInvalidSparseVoxelIndex || metadata.owningPageIndex >= pageStates_.size())
    {
      recordMissingSelection();
      continue;
    }

    if (node.childCount == 0u)
    {
      if (metadata.refinementPageIndex != kInvalidSparseVoxelIndex)
      {
        if (metadata.refinementPageIndex >= pageStates_.size())
        {
          recordMissingSelection();
          continue;
        }
        const auto childState = pageStates_[metadata.refinementPageIndex].state;
        if (childState == SparseVoxelPageState::Resident || childState == SparseVoxelPageState::EvictPending)
        {
          recordMissingSelection();
          continue;
        }
        ++diagnostics.parentFallbackCount;
        ++diagnostics.pageResidencyMissCount;
      }
      continue;
    }

    if (node.firstChild >= physicalNodeCapacity_ || node.childCount > physicalNodeCapacity_ - node.firstChild)
    {
      recordMissingSelection();
      continue;
    }
    ++diagnostics.childSelectionCount;
    for (uint32_t child = 0u; child < node.childCount; ++child)
      stack.push_back(node.firstChild + child);
  }

  if (!valid)
    return fail(error, "sparse voxel selected neither parent nor child");
  if (error != nullptr)
    error->clear();
  return true;
}

uint32_t SparseVoxelPageResidency::largestFreeNodeRange() const
{
  uint32_t result = 0u;
  for (const FreeRange range : freeRanges_)
    result = std::max(result, range.count);
  return result;
}

uint32_t SparseVoxelPageResidency::logicalToPhysical(const uint32_t logicalNodeIndex) const
{
  return logicalNodeIndex < logicalToPhysical_.size() ? logicalToPhysical_[logicalNodeIndex] : kInvalidSparseVoxelIndex;
}

uint32_t SparseVoxelPageResidency::refinementPageForLogicalNode(const uint32_t logicalNodeIndex) const
{
  return logicalNodeIndex < refinementPages_.size() ? refinementPages_[logicalNodeIndex] : kInvalidSparseVoxelIndex;
}

std::vector<uint32_t> SparseVoxelPageResidency::changedPagesSince(const uint64_t revision) const
{
  std::vector<uint32_t> changed;
  if (revision >= contentRevision_)
    return changed;
  changed.reserve(static_cast<size_t>(contentRevision_ - revision));
  for (uint32_t pageIndex = 0u; pageIndex < pageContentRevisions_.size(); ++pageIndex)
    if (pageContentRevisions_[pageIndex] > revision)
      changed.push_back(pageIndex);
  return changed;
}

SparseVoxelAsyncPageReader::~SparseVoxelAsyncPageReader()
{
  for (auto &pending : pending_)
    pending.future.wait();
}

bool SparseVoxelAsyncPageReader::initialize(std::string path, VirtualSparseVoxelSceneMetadata metadata, const uint64_t generation, const uint32_t maxInFlight, std::string *error)
{
  for (auto &pending : pending_)
    pending.future.wait();
  pending_.clear();
  if (path.empty() || metadata.pages.empty() || maxInFlight == 0u)
  {
    if (error != nullptr)
      *error = "sparse voxel async reader requires a path, metadata, and non-zero queue capacity";
    return false;
  }
  path_ = std::move(path);
  metadata_ = std::move(metadata);
  generation_ = generation;
  maxInFlight_ = maxInFlight;
  if (error != nullptr)
    error->clear();
  return true;
}

bool SparseVoxelAsyncPageReader::enqueue(const uint32_t pageIndex, std::string *error)
{
  if (maxInFlight_ == 0u || pageIndex >= metadata_.pages.size())
  {
    if (error != nullptr)
      *error = "sparse voxel async page request is invalid";
    return false;
  }
  if (pending_.size() >= maxInFlight_)
  {
    if (error != nullptr)
      *error = "sparse voxel async read queue is full";
    return false;
  }
  for (const auto &pending : pending_)
  {
    if (pending.generation == generation_ && pending.pageIndex == pageIndex)
    {
      if (error != nullptr)
        *error = "sparse voxel page read is already in flight";
      return false;
    }
  }
  const uint64_t generation = generation_;
  pending_.push_back({
    generation,
    pageIndex,
    std::async(
        std::launch::async,
        [this, generation, pageIndex]()
        {
          SparseVoxelPageReadCompletion completion{};
          completion.generation = generation;
          completion.pageIndex = pageIndex;
          completion.success = readVirtualSparseVoxelPage(path_, metadata_, pageIndex, completion.page, &completion.error);
          return completion;
        }),
  });
  if (error != nullptr)
    error->clear();
  return true;
}

std::vector<SparseVoxelPageReadCompletion> SparseVoxelAsyncPageReader::poll()
{
  std::vector<SparseVoxelPageReadCompletion> result;
  for (size_t index = 0u; index < pending_.size();)
  {
    auto &pending = pending_[index];
    if (pending.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    {
      ++index;
      continue;
    }
    auto completion = pending.future.get();
    completion.stale = completion.generation != generation_;
    if (completion.stale)
    {
      completion.success = false;
      completion.page = {};
      completion.error = "stale sparse voxel page completion";
    }
    result.push_back(std::move(completion));
    pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(index));
  }
  std::sort(
      result.begin(),
      result.end(),
      [](const SparseVoxelPageReadCompletion &a, const SparseVoxelPageReadCompletion &b)
      {
        return a.pageIndex < b.pageIndex;
      });
  return result;
}

void SparseVoxelAsyncPageReader::cancelAndAdvanceGeneration()
{
  ++generation_;
}

} // namespace voxel::svraster
