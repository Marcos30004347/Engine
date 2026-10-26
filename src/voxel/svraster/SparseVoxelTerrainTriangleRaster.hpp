#pragma once

#include "voxel/svraster/SparseVoxelStreaming.hpp"
#include "voxel/svraster/SparseVoxelTerrainTriangleMesh.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace voxel::svraster
{

struct SparseVoxelTerrainTriangleDrawIndexedIndirectGPU
{
  uint32_t indexCount = 0u;
  uint32_t instanceCount = 0u;
  uint32_t firstIndex = 0u;
  int32_t vertexOffset = 0;
  uint32_t firstInstance = 0u;
};

static_assert(
    sizeof(SparseVoxelTerrainTriangleDrawIndexedIndirectGPU) == 20u,
    "must match VkDrawIndexedIndirectCommand");

enum class SparseVoxelTerrainRasterPath : uint32_t
{
  None = 0u,
  BrickFallback,
  TriangleClusters,
};

struct SparseVoxelTerrainTriangleRasterPageView
{
  SparseVoxelPageId page{};
  uint64_t geometryRevision = 0u;
  const SparseVoxelTerrainTrianglePageSidecar *sidecar = nullptr;
};

struct SparseVoxelTerrainTriangleDrawRecord
{
  SparseVoxelPageId page{};
  uint32_t instanceIndex = kInvalidSparseVoxelIndex;
  uint32_t clusterIndex = kInvalidSparseVoxelIndex;
  uint32_t lodGroup = kSparseVoxelBrickLODGroupInvalidIndex;
  std::array<float, 3> boundsMin{};
  std::array<float, 3> boundsMax{};
  SparseVoxelTerrainTriangleDrawIndexedIndirectGPU indirect{};
};

struct SparseVoxelTerrainRasterSelection
{
  uint32_t instanceIndex = kInvalidSparseVoxelIndex;
  SparseVoxelPageId desiredPage{};
  SparseVoxelPageId selectedPage{};
  SparseVoxelTerrainRasterPath path = SparseVoxelTerrainRasterPath::None;
  bool selectedFallback = false;
  bool requestedDesiredPage = false;
  uint32_t firstDraw = 0u;
  uint32_t drawCount = 0u;
};

struct SparseVoxelTerrainTriangleRasterDrawList
{
  std::vector<SparseVoxelTerrainRasterSelection> selections{};
  std::vector<SparseVoxelTerrainTriangleDrawRecord> triangleDraws{};
  uint32_t triangleSelectionCount = 0u;
  uint32_t brickFallbackCount = 0u;
  uint32_t missingRasterCount = 0u;
  uint32_t staleSidecarCount = 0u;
};

struct SparseVoxelTerrainTriangleFrustumPlane
{
  std::array<float, 3> normal{};
  float distance = 0.0f;
};

struct SparseVoxelTerrainTriangleCullSettings
{
  bool enableFrustum = true;
  bool enableHZB = false;
  bool cameraCut = false;
  // Optional Niagara-style occluder seed. A non-zero stride marks every Nth
  // in-frustum cluster for the early pass even if it was not visible last
  // frame. This keeps the CPU oracle deterministic; the GPU path can replace
  // it with an occluder-score buffer.
  uint32_t occluderStride = 0u;
  uint32_t frustumPlaneCount = 0u;
  std::array<SparseVoxelTerrainTriangleFrustumPlane, 6> frustumPlanes{};
  std::vector<uint8_t> previouslyVisible{};
  std::vector<uint8_t> hzbOccluded{};
};

struct SparseVoxelTerrainTriangleCullPlan
{
  std::vector<uint32_t> earlyDrawIndices{};
  std::vector<uint32_t> lateDrawIndices{};
  std::vector<uint32_t> rejectedDrawIndices{};
  std::vector<SparseVoxelTerrainTriangleDrawIndexedIndirectGPU> earlyIndirect{};
  std::vector<SparseVoxelTerrainTriangleDrawIndexedIndirectGPU> lateIndirect{};
  uint32_t inputDrawCount = 0u;
  uint32_t submittedDrawCount = 0u;
  uint32_t rejectedDrawCount = 0u;
  uint32_t frustumRejectedCount = 0u;
  uint32_t hzbRejectedCount = 0u;
  uint32_t previousVisibleEarlyCount = 0u;
  uint32_t occluderEarlyCount = 0u;
  uint64_t submittedIndexCount = 0u;
  uint64_t rejectedIndexCount = 0u;
  uint64_t submittedTriangleCount = 0u;
  uint64_t rejectedTriangleCount = 0u;
};

bool buildSparseVoxelTerrainTriangleRasterDrawList(
    const SparseVoxelSharedPageTable &table,
    const std::vector<SparseVoxelTraversalCandidate> &candidates,
    const std::vector<SparseVoxelTerrainTriangleRasterPageView> &trianglePages,
    const SparseVoxelTerrainTriangleMeshSettings &settings,
    SparseVoxelTerrainTriangleRasterDrawList &result,
    std::string *error = nullptr);

// CPU oracle for renderer tests only. Runtime terrain triangle raster must use
// the virtual-geometry GPU cut/culling/draw path instead of this helper.
bool buildSparseVoxelTerrainTriangleCullPlan(
    const SparseVoxelTerrainTriangleRasterDrawList &drawList,
    const SparseVoxelTerrainTriangleCullSettings &settings,
    SparseVoxelTerrainTriangleCullPlan &result,
    std::string *error = nullptr);

} // namespace voxel::svraster
