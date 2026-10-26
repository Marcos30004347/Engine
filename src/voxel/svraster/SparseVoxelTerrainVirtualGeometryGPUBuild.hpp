#pragma once

#include "voxel/svraster/SparseVoxelStreaming.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace voxel::svraster
{

enum class SparseVoxelTerrainVGPUPageBuildStatus : uint32_t
{
  Empty = 0u,
  Ready = 1u,
  Rejected = 2u,
};

constexpr uint32_t kSparseVoxelTerrainVGPUStageSourceGenerated = 1u << 0u;
constexpr uint32_t kSparseVoxelTerrainVGPUStageSurfaceClassified = 1u << 1u;
constexpr uint32_t kSparseVoxelTerrainVGPUStageMeshletPrefixed = 1u << 2u;
constexpr uint32_t kSparseVoxelTerrainVGPUStageMeshletEmitted = 1u << 3u;
constexpr uint32_t kSparseVoxelTerrainVGPUStageVGPageEncoded = 1u << 4u;
constexpr uint32_t kSparseVoxelTerrainVGPUStageTraceClassified = 1u << 5u;
constexpr uint32_t kSparseVoxelTerrainVGPUStageTraceBuilt = 1u << 6u;
constexpr uint32_t kSparseVoxelTerrainVGPUStageDependenciesBuilt = 1u << 7u;
constexpr uint32_t kSparseVoxelTerrainVGPUStagePageValidated = 1u << 8u;
constexpr uint32_t kSparseVoxelTerrainVGPURequiredStages =
    kSparseVoxelTerrainVGPUStageSourceGenerated |
    kSparseVoxelTerrainVGPUStageSurfaceClassified |
    kSparseVoxelTerrainVGPUStageMeshletPrefixed |
    kSparseVoxelTerrainVGPUStageMeshletEmitted |
    kSparseVoxelTerrainVGPUStageVGPageEncoded |
    kSparseVoxelTerrainVGPUStageTraceClassified |
    kSparseVoxelTerrainVGPUStageTraceBuilt |
    kSparseVoxelTerrainVGPUStageDependenciesBuilt |
    kSparseVoxelTerrainVGPUStagePageValidated;

struct SparseVoxelTerrainVGPUPageBuildSettings
{
  uint32_t packedPageSlotBytes = kSparseVoxelPackedPageSlotBytes;
  uint32_t maximumVGMeshletCount = 65536u;
  uint32_t maximumVGHierarchyNodeCount = 1u << 20u;
  uint32_t maximumVGGroupCount = 65536u;
  uint32_t maximumVGVertexCount = 4u * 65536u;
  uint32_t maximumVGTriangleCount = 2u * 65536u;
  uint32_t maximumTraceNodeCount = 1u << 20u;
  uint32_t maximumDependencyCount = 64u;
  bool requireVGRasterPayload = true;
  bool requireTracePayload = true;
  bool requireTraceErrorSidecar = true;
  bool requireDependencyMetadata = true;
  bool requireMaterialPayload = false;
  // Default generated terrain VG pages preserve one quad/two triangles per
  // exposed voxel face, with private vertices for each face.  Greedy merging
  // may be benchmarked by disabling this, but the shipping VG contract keeps
  // normals, UV unwraps and per-face colour independent.
  bool requireFaceLocalVoxelFaces = true;
};

struct SparseVoxelTerrainVGPUPageBuildResult
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
  uint32_t status = static_cast<uint32_t>(SparseVoxelTerrainVGPUPageBuildStatus::Empty);
  uint32_t stageFlags = 0u;
  uint32_t validationErrors = 0u;
  uint32_t sourceVoxelCount = 0u;
  uint32_t exposedFaceCount = 0u;
  uint32_t greedyQuadCount = 0u;
  uint32_t vgMeshletCount = 0u;
  uint32_t vgGroupCount = 0u;
  uint32_t vgHierarchyNodeCount = 0u;
  uint32_t vgInstallRewriteCount = 0u;
  uint32_t vgUninstallRewriteCount = 0u;
  uint32_t vgVertexCount = 0u;
  uint32_t vgTriangleCount = 0u;
  uint32_t vgMaxVerticesPerMeshlet = 0u;
  uint32_t vgMaxTrianglesPerMeshlet = 0u;
  uint32_t vgPositionWordCount = 0u;
  uint32_t vgNormalWordCount = 0u;
  uint32_t vgUVWordCount = 0u;
  uint32_t vgIndexByteCount = 0u;
  uint32_t vgBoneWeightWordCount = 0u;
  uint32_t traceFormat = kVoxelTraceFormatInvalid;
  uint32_t traceNodeCount = 0u;
  uint32_t traceErrorWordCount = 0u;
  uint32_t cullErrorWordCount = 0u;
  uint32_t materialCount = 0u;
  uint32_t materialCellCount = 0u;
  uint32_t dependencyCount = 0u;
  bool rootPage = false;
};

struct SparseVoxelTerrainVGPUPageBuildValidation
{
  bool ready = false;
  bool completeVGRasterPayload = false;
  bool completeTracePayload = false;
  bool completeDependencyPayload = false;
  bool fitsPackedPageSlot = false;
  uint64_t vgRasterPayloadBytes = 0u;
  uint64_t tracePayloadBytes = 0u;
  uint64_t materialPayloadBytes = 0u;
  uint64_t totalPayloadBytes = 0u;
};

struct SparseVoxelTerrainVGPUPagePublication
{
  bool validated = false;
  bool reserved = false;
  bool copied = false;
  bool committed = false;
  SparseVoxelProducedPagePayload produced{};
  SparseVoxelReservedPagePayload reservation{};
};

uint64_t sparseVoxelTerrainVGPUPageRasterBytes(
    const SparseVoxelTerrainVGPUPageBuildResult &build);

uint64_t sparseVoxelTerrainVGPUPageTraceBytes(
    const SparseVoxelTerrainVGPUPageBuildResult &build);

bool validateSparseVoxelTerrainVGPUPageBuild(
    const SparseVoxelTerrainVGPUPageBuildResult &build,
    const SparseVoxelTerrainVGPUPageBuildSettings &settings,
    SparseVoxelTerrainVGPUPageBuildValidation &validation,
    std::string *error = nullptr);

bool sparseVoxelProducedPagePayloadFromTerrainVGPUPageBuild(
    const SparseVoxelTerrainVGPUPageBuildResult &build,
    const SparseVoxelTerrainVGPUPageBuildSettings &settings,
    SparseVoxelProducedPagePayload &payload,
    std::string *error = nullptr);

bool prepareSparseVoxelTerrainVGPUPageBuildPublication(
    SparseVoxelSharedPageTable &table,
    const SparseVoxelTerrainVGPUPageBuildResult &build,
    const SparseVoxelTerrainVGPUPageBuildSettings &settings,
    uint64_t copyFence,
    SparseVoxelTerrainVGPUPagePublication &publication,
    std::string *error = nullptr);

bool publishSparseVoxelTerrainVGPUPageBuild(
    SparseVoxelSharedPageTable &table,
    const SparseVoxelTerrainVGPUPageBuildResult &build,
    const SparseVoxelTerrainVGPUPageBuildSettings &settings,
    uint64_t copyFence,
    const std::vector<SparseVoxelPageInstallRewrite> &rewrites,
    SparseVoxelTerrainVGPUPagePublication &publication,
    std::string *error = nullptr);

} // namespace voxel::svraster
