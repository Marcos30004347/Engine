#pragma once

#include "voxel/svraster/SparseVoxelStreaming.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace voxel::svraster
{

enum class SparseVoxelTerrainTriangleGPUPageBuildStatus : uint32_t
{
  Empty = 0u,
  Ready = 1u,
  Rejected = 2u,
};

constexpr uint32_t kSparseVoxelTerrainTriangleGPUBuildSourceGenerated = 1u << 0u;
constexpr uint32_t kSparseVoxelTerrainTriangleGPUBuildTriangleClassified = 1u << 1u;
constexpr uint32_t kSparseVoxelTerrainTriangleGPUBuildTrianglePrefixed = 1u << 2u;
constexpr uint32_t kSparseVoxelTerrainTriangleGPUBuildTriangleEmitted = 1u << 3u;
constexpr uint32_t kSparseVoxelTerrainTriangleGPUBuildTraceBuilt = 1u << 4u;
constexpr uint32_t kSparseVoxelTerrainTriangleGPUBuildDependenciesBuilt = 1u << 5u;
constexpr uint32_t kSparseVoxelTerrainTriangleGPUBuildPageValidated = 1u << 6u;
constexpr uint32_t kSparseVoxelTerrainTriangleGPUBuildRequiredStages =
    kSparseVoxelTerrainTriangleGPUBuildSourceGenerated |
    kSparseVoxelTerrainTriangleGPUBuildTriangleClassified |
    kSparseVoxelTerrainTriangleGPUBuildTrianglePrefixed |
    kSparseVoxelTerrainTriangleGPUBuildTriangleEmitted |
    kSparseVoxelTerrainTriangleGPUBuildTraceBuilt |
    kSparseVoxelTerrainTriangleGPUBuildDependenciesBuilt |
    kSparseVoxelTerrainTriangleGPUBuildPageValidated;

struct SparseVoxelTerrainTriangleGPUPageBuildSettings
{
  uint32_t packedPageSlotBytes = kSparseVoxelPackedPageSlotBytes;
  uint32_t maximumTriangleClusterCount = 65536u;
  uint32_t maximumTriangleVertexCount = 4u * 65536u;
  uint32_t maximumTriangleIndexCount = 6u * 65536u;
  uint32_t maximumTraceNodeCount = 1u << 20u;
  uint32_t maximumDependencyCount = 64u;
  bool requireTrianglePayload = true;
  bool requireTracePayload = true;
  bool requireTraceErrorSidecar = true;
  bool requireDependencyMetadata = true;
  bool requireMaterialPayload = false;
  bool requireFaceLocalVoxelFaces = true;
};

struct SparseVoxelTerrainTriangleGPUPageBuildResult
{
  SparseVoxelPageId page{};
  SparseVoxelPageId parentPage{};
  SparseVoxelPageId fallbackPage{};
  uint64_t expectedPageGeneration = 0u;
  uint32_t requiredProducerRevision = 0u;
  uint64_t payloadRevision = 0u;
  uint64_t geometryRevision = 0u;
  uint64_t materialRevision = 0u;
  uint64_t productionFence = 0u;
  uint32_t terrainGeneration = 0u;
  uint32_t buildRevision = 0u;
  uint32_t status = static_cast<uint32_t>(SparseVoxelTerrainTriangleGPUPageBuildStatus::Empty);
  uint32_t stageFlags = 0u;
  uint32_t validationErrors = 0u;
  uint32_t sourceVoxelCount = 0u;
  uint32_t exposedFaceCount = 0u;
  uint32_t greedyQuadCount = 0u;
  uint32_t hierarchyNodeCount = 0u;
  uint32_t childIndexCount = 0u;
  uint32_t triangleClusterCount = 0u;
  uint32_t triangleVertexCount = 0u;
  uint32_t triangleIndexCount = 0u;
  uint32_t traceFormat = kVoxelTraceFormatInvalid;
  uint32_t traceNodeCount = 0u;
  uint32_t traceErrorWordCount = 0u;
  uint32_t cullErrorWordCount = 0u;
  uint32_t materialCount = 0u;
  uint32_t materialCellCount = 0u;
  uint32_t dependencyCount = 0u;
  uint32_t installRewriteCount = 1u;
  uint32_t uninstallRewriteCount = 1u;
  bool rootPage = false;
};

struct SparseVoxelTerrainTriangleGPUPageBuildValidation
{
  bool ready = false;
  bool completeRasterPayload = false;
  bool completeTracePayload = false;
  bool completeDependencyPayload = false;
  bool fitsPackedPageSlot = false;
  uint64_t rasterPayloadBytes = 0u;
  uint64_t tracePayloadBytes = 0u;
  uint64_t materialPayloadBytes = 0u;
  uint64_t totalPayloadBytes = 0u;
};

struct SparseVoxelTerrainTriangleGPUPagePublication
{
  bool validated = false;
  bool reserved = false;
  bool copied = false;
  bool committed = false;
  SparseVoxelProducedPagePayload produced{};
  SparseVoxelReservedPagePayload reservation{};
};

uint64_t sparseVoxelTerrainTriangleGPUPageRasterBytes(
    const SparseVoxelTerrainTriangleGPUPageBuildResult &build);

uint64_t sparseVoxelTerrainTriangleGPUPageTraceBytes(
    const SparseVoxelTerrainTriangleGPUPageBuildResult &build);

bool validateSparseVoxelTerrainTriangleGPUPageBuild(
    const SparseVoxelTerrainTriangleGPUPageBuildResult &build,
    const SparseVoxelTerrainTriangleGPUPageBuildSettings &settings,
    SparseVoxelTerrainTriangleGPUPageBuildValidation &validation,
    std::string *error = nullptr);

bool sparseVoxelProducedPagePayloadFromTerrainTriangleGPUBuild(
    const SparseVoxelTerrainTriangleGPUPageBuildResult &build,
    const SparseVoxelTerrainTriangleGPUPageBuildSettings &settings,
    SparseVoxelProducedPagePayload &payload,
    std::string *error = nullptr);

bool prepareSparseVoxelTerrainTriangleGPUPageBuildPublication(
    SparseVoxelSharedPageTable &table,
    const SparseVoxelTerrainTriangleGPUPageBuildResult &build,
    const SparseVoxelTerrainTriangleGPUPageBuildSettings &settings,
    uint64_t copyFence,
    SparseVoxelTerrainTriangleGPUPagePublication &publication,
    std::string *error = nullptr);

bool publishSparseVoxelTerrainTriangleGPUPageBuild(
    SparseVoxelSharedPageTable &table,
    const SparseVoxelTerrainTriangleGPUPageBuildResult &build,
    const SparseVoxelTerrainTriangleGPUPageBuildSettings &settings,
    uint64_t copyFence,
    const std::vector<SparseVoxelPageInstallRewrite> &rewrites,
    SparseVoxelTerrainTriangleGPUPagePublication &publication,
    std::string *error = nullptr);

} // namespace voxel::svraster
