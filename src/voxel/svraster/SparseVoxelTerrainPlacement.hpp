#pragma once

#include "procedural/world/MapPlacement.hpp"
#include "voxel/svraster/SparseVoxelStreaming.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace voxel::svraster
{

enum class SparseVoxelTerrainPlacementResidencyDomain : uint8_t
{
  SparseVoxel,
  VirtualGeometry,
};

struct SparseVoxelTerrainPlacementCandidateBufferKey
{
  procedural::world::MapExportPageAddress worldDataPage{};
  SparseVoxelPageId terrainPage{};
  uint64_t worldSeed = 0u;
  uint32_t placementPaletteVersion = 0u;
  uint64_t placementPaletteSignature = 0u;
  uint64_t terrainOutputRevision = 0u;

  bool operator==(const SparseVoxelTerrainPlacementCandidateBufferKey &other) const;
  bool operator<(const SparseVoxelTerrainPlacementCandidateBufferKey &other) const;
};

struct SparseVoxelTerrainPlacementFootprintExclusion
{
  // Produced by already-accepted instances, authored objects, gameplay
  // blockers, or another placement page. Terrain consumes only this opaque
  // footprint feedback; it does not load or inspect the source asset.
  uint64_t sourceStableId = 0u;
  procedural::Point2 center{};
  double radiusMetres = 0.0;
};

struct SparseVoxelTerrainPlacementBuildSettings
{
  bool enabled = true;
  double candidateCellMetres = 16.0;
  size_t maximumEvaluatedSites = 1u << 20u;
  size_t maximumCandidates = 1u << 18u;
  std::vector<SparseVoxelTerrainPlacementFootprintExclusion> footprintExclusions{};
};

struct SparseVoxelTerrainPlacementCandidateBuffer
{
  SparseVoxelTerrainPlacementCandidateBufferKey key{};
  std::vector<procedural::world::PlacementCandidate> candidates{};
  size_t evaluatedSites = 0u;
  size_t densityRejected = 0u;
  size_t spacingRejected = 0u;
  size_t waterRejected = 0u;
  size_t mapExclusionRejected = 0u;
  size_t footprintRejected = 0u;
  size_t ancestorSampledSites = 0u;
};

struct SparseVoxelTerrainPlacementAssetResidency
{
  std::string assetKey;
  SparseVoxelTerrainPlacementResidencyDomain domain = SparseVoxelTerrainPlacementResidencyDomain::VirtualGeometry;

  SparseVoxelPageProducer sparseVoxelProducer{};
  SparseVoxelPageId sparseVoxelRootPage{};
  SparseVoxelPageId sparseVoxelDesiredPage{};
  uint32_t sparseVoxelFirstRootNode = kInvalidSparseVoxelIndex;
  uint32_t sparseVoxelRootNodeCount = 0u;
  uint32_t sparseVoxelFirstPage = 0u;
  uint32_t sparseVoxelPageCount = 0u;

  uint32_t virtualGeometryObjectIndex = kInvalidSparseVoxelIndex;
  uint32_t virtualGeometryRootPage = kInvalidSparseVoxelIndex;
  uint32_t virtualGeometryDesiredPage = kInvalidSparseVoxelIndex;
  uint32_t virtualGeometryPageTableOffset = kInvalidSparseVoxelIndex;
};

struct SparseVoxelTerrainPlacementInstanceRequest
{
  uint64_t stableInstanceId = 0u;
  SparseVoxelTerrainPlacementCandidateBufferKey candidateKey{};
  uint32_t candidateIndex = kInvalidSparseVoxelIndex;
  uint32_t instanceIndex = kInvalidSparseVoxelIndex;
  std::string assetKey;
  std::string collectionKey;
  std::string resourceKey;
  SparseVoxelTerrainPlacementResidencyDomain domain = SparseVoxelTerrainPlacementResidencyDomain::VirtualGeometry;
  SparseVoxelStreamedInstance sparseVoxelInstance{};
  SparseVoxelPageId sparseVoxelDesiredPage{};
  uint32_t virtualGeometryObjectIndex = kInvalidSparseVoxelIndex;
  uint32_t virtualGeometryRootPage = kInvalidSparseVoxelIndex;
  uint32_t virtualGeometryDesiredPage = kInvalidSparseVoxelIndex;
  uint32_t virtualGeometryPageTableOffset = kInvalidSparseVoxelIndex;
  std::array<float, 16> modelToWorld{};
  float footprintRadiusMetres = 0.0f;
  bool ownsAssetPayload = false;
};

uint64_t sparseVoxelTerrainPlacementPaletteSignature(const procedural::world::PlacementPalette &palette);

bool buildSparseVoxelTerrainPlacementCandidateBuffer(
    const procedural::world::MapExportBundle &bundle,
    const procedural::world::PlacementPalette &palette,
    const SparseVoxelTerrainPlacementCandidateBufferKey &key,
    const SparseVoxelTerrainPlacementBuildSettings &settings,
    SparseVoxelTerrainPlacementCandidateBuffer &buffer,
    std::string *error = nullptr);

bool buildSparseVoxelTerrainPlacementInstanceRequests(
    const SparseVoxelTerrainPlacementCandidateBuffer &buffer,
    const procedural::world::PlacementPalette &palette,
    const std::vector<SparseVoxelTerrainPlacementAssetResidency> &residency,
    uint32_t firstInstanceIndex,
    std::vector<SparseVoxelTerrainPlacementInstanceRequest> &requests,
    std::string *error = nullptr);

} // namespace voxel::svraster
