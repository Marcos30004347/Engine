#include "voxel/svraster/TerrainGPUV2RasterHierarchy.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <set>
#include <string>
#include <vector>

namespace svr = voxel::svraster;

namespace
{
int failures = 0;
void check(const bool condition, const std::string &message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

struct Fixture
{
  svr::TerrainGPUV2FineRasterConfigGPU rasterConfig{};
  std::vector<svr::TerrainGPUV2HeightSampleGPU> heights{};
  svr::TerrainGPUV2FineRasterOutput raster{};
  svr::TerrainGPUV2RasterHierarchyConfigGPU config{};
};

Fixture fixture(
    const uint32_t clipmapLOD = 0u,
    const uint32_t generation = 13u,
    const bool flatContinuousHeightRegression = false)
{
  Fixture value{};
  const svr::TerrainGPUChunkShape shape{4u, 2u, 4u, 8u};
  const svr::TerrainGPUChunkKey key{51u, 0, 0, 0, static_cast<uint16_t>(clipmapLOD), 1u, 9u, generation};
  svr::TerrainGPUV2ColumnConfigGPU columnsConfig{};
  columnsConfig.columnsX = 32u;
  columnsConfig.columnsZ = 32u;
  columnsConfig.halo = 2u;
  columnsConfig.heightUnitMetres = 0.5f;
  columnsConfig.canonicalStep = 1u << clipmapLOD;
  columnsConfig.expectedTerrainGeneration = generation;
  svr::TerrainGPUV2FineRasterSettings rasterSettings{};
  rasterSettings.key = key;
  rasterSettings.shape = shape;
  rasterSettings.columns = columnsConfig;
  rasterSettings.capacity = {32u, 16384u};
  rasterSettings.finestVoxelSizeMetres = 0.5;
  rasterSettings.regionMappingId = 23u;
  rasterSettings.paletteBase = 5u;
  std::string error;
  check(svr::planTerrainGPUV2FineRaster(rasterSettings, value.rasterConfig, &error), "Phase 6 raster fixture plans: " + error);
  const uint32_t stride = columnsConfig.columnsX + 2u * columnsConfig.halo;
  const uint32_t sampleStride = stride + 1u;
  value.heights.resize(uint64_t{sampleStride} *
      (columnsConfig.columnsZ + 2u * columnsConfig.halo + 1u));
  for (uint32_t z = 0u; z < columnsConfig.columnsZ + 2u * columnsConfig.halo + 1u; ++z)
    for (uint32_t x = 0u; x < sampleStride; ++x)
    {
      const int32_t localX = static_cast<int32_t>(x) -
          static_cast<int32_t>(columnsConfig.halo);
      const int32_t localZ = static_cast<int32_t>(z) -
          static_cast<int32_t>(columnsConfig.halo);
      const int32_t localHeight = flatContinuousHeightRegression
          ? 8
          : 2 + static_cast<int32_t>(
                    ((std::max(localX, 0) * 7 + std::max(localZ, 0) * 11 +
                         (std::max(localX, 0) / 5) * 3) %
                        14));
      const int32_t heightUnits = value.rasterConfig.chunkMinimumCanonicalY +
          localHeight * static_cast<int32_t>(columnsConfig.canonicalStep);
      const double residual = flatContinuousHeightRegression
          ? 0.875
          : (((x + 2u * z) % 5u == 0u) ? 0.75 : 0.125);
      const double heightMetres =
          (static_cast<double>(heightUnits) - residual) *
          columnsConfig.heightUnitMetres;
      value.heights[uint64_t{z} * sampleStride + x] = {
          heightUnits,
          static_cast<int32_t>(
              value.rasterConfig.chunkMinimumCanonicalX +
              localX * static_cast<int32_t>(columnsConfig.canonicalStep)),
          static_cast<int32_t>(
              value.rasterConfig.chunkMinimumCanonicalZ +
              localZ * static_cast<int32_t>(columnsConfig.canonicalStep)),
          svr::terrainGPUV2PackHeightSampleFlags(
              svr::TerrainGPUV2ColumnValid,
              heightMetres,
              columnsConfig.heightUnitMetres,
              heightUnits)};
    }
  std::vector<svr::TerrainGPUV2ColumnGPU> columns(uint64_t{stride} * (columnsConfig.columnsZ + 2u * columnsConfig.halo));
  for (auto &column : columns)
    column = {0, 0, 0u, svr::TerrainGPUV2ColumnValid};
  for (uint32_t z = 0u; z < 32u; ++z)
    for (uint32_t x = 0u; x < 32u; ++x)
    {
      auto &column = columns[uint64_t{z + 2u} * stride + x + 2u];
      const int32_t localHeight = flatContinuousHeightRegression
          ? 8
          : 2 + static_cast<int32_t>((x * 7u + z * 11u + (x / 5u) * 3u) % 14u);
      column.maximumHeightUnits = value.rasterConfig.chunkMinimumCanonicalY + localHeight * static_cast<int32_t>(columnsConfig.canonicalStep);
      column.material = flatContinuousHeightRegression
          ? 7u
          : 1u + (x / 4u + z / 3u) % 29u;
    }
  svr::TerrainGPUV2ColumnResultGPU columnResult{};
  columnResult.status = static_cast<uint32_t>(svr::TerrainGPUV2ColumnStatus::Ready);
  columnResult.terrainGeneration = generation;
  columnResult.columnCount = static_cast<uint32_t>(columns.size());
  columnResult.writtenColumns = static_cast<uint32_t>(columns.size());
  check(svr::buildTerrainGPUV2FineRasterOracle(value.rasterConfig, columnResult, columns, value.raster, &error), "Phase 6 fine raster oracle builds: " + error);
  svr::TerrainGPUV2RasterHierarchySettings hierarchySettings{};
  hierarchySettings.capacity = {37u, 18944u, 40u, 1u};
  check(svr::planTerrainGPUV2RasterHierarchy(value.rasterConfig, hierarchySettings, value.config, &error), "Phase 6 hierarchy fixture plans: " + error);
  return value;
}

svr::TerrainGPUV2RasterHierarchyOutput build(const Fixture &source)
{
  svr::TerrainGPUV2RasterHierarchyOutput output{};
  std::string error;
  check(svr::buildTerrainGPUV2RasterHierarchyOracle(source.config, source.raster.result, source.raster.nodes, source.raster.occupancy, source.raster.materialWords, source.heights, output, &error),
      "Phase 6 hierarchy oracle builds: " + error);
  check(output.result.status == static_cast<uint32_t>(svr::TerrainGPUV2RasterHierarchyStatus::Ready) && svr::validateTerrainGPUV2RasterHierarchy(source.config, output, &error),
      "Phase 6 hierarchy validates: " + error);
  return output;
}

void abiBudgetAndQualification()
{
  const auto source = fixture();
  check(sizeof(svr::TerrainGPUV2RasterHierarchyConfigGPU) == 320u && sizeof(svr::TerrainGPUV2RasterHierarchyResultGPU) == 64u, "raster hierarchy GPU ABIs are frozen");
  check(source.config.levelCount == 3u && source.config.levelDenseCounts[0] == 32u && source.config.levelDenseCounts[1] == 4u && source.config.levelDenseCounts[2] == 1u,
      "4x2x4 leaves reduce LOD0-to-root in independent 3D levels");
  svr::TerrainGPUV2RasterHierarchyMemory memory{};
  std::string error;
  check(svr::terrainGPUV2RasterHierarchyMemory(source.config, memory, &error) && memory.scratchBytes != 0u && memory.outputBytes != 0u && memory.microGeometryBytes == 0u,
      "hierarchy scratch/output are preflighted and microgeometry remains zero: " + error);
  auto insufficient = svr::TerrainGPUV2RasterHierarchySettings{{36u, 18944u, 40u, 1u}};
  svr::TerrainGPUV2RasterHierarchyConfigGPU ignored{};
  check(!svr::planTerrainGPUV2RasterHierarchy(source.rasterConfig, insufficient, ignored, &error), "worst-case hierarchy capacity is rejected before GPU writes");

  svr::TerrainGPUV2RasterHierarchyQualification qualification{};
  qualification.experimentEnabled = true;
  qualification.deviceQualified = true;
  qualification.minimumImprovementPermille = 50u;
  qualification.leafOnlyEndToEndP99Microseconds = 1000u;
  qualification.hierarchyEndToEndP99Microseconds = 940u;
  check(svr::selectTerrainGPUV2RasterMode(qualification) == svr::TerrainGPUV2RasterMode::Hierarchy, "device mode enables hierarchy only after a qualifying p99 improvement");
  qualification.hierarchyEndToEndP99Microseconds = 960u;
  check(svr::selectTerrainGPUV2RasterMode(qualification) == svr::TerrainGPUV2RasterMode::LeafOnly, "non-improving hierarchy remains disabled by default");

  const svr::TerrainGPUV2RasterHierarchyCapture leafCapture{120u, 1000u, 10000u, 32u << 20u};
  const svr::TerrainGPUV2RasterHierarchyCapture hierarchyCapture{260u, 940u, 4000u, 40u << 20u};
  const auto comparison = svr::compareTerrainGPUV2RasterHierarchyCaptures(leafCapture, hierarchyCapture);
  check(
      comparison.renderNodeSavingsPermille == 600u && comparison.buildP99DeltaMicroseconds == 140 &&
          comparison.endToEndImprovementPermille == 60 && comparison.residentByteDelta == int64_t{8u << 20u},
      "capture comparison reports render savings against build and resident-memory cost");
}

void hierarchyAndCuts()
{
  const auto source = fixture();
  const auto output = build(source);
  bool monotonic = output.result.nodeCount > output.result.leafNodeCount && output.rootIndices.size() == 1u;
  bool measuredRepresentativeError = false;
  for (uint32_t parentIndex = output.result.leafNodeCount; parentIndex < output.nodes.size(); ++parentIndex)
  {
    const auto &parent = output.nodes[parentIndex];
    monotonic &= parent.childCount != 0u && parent.childCount <= 8u;
    float childMaximumError = 0.0f;
    for (uint32_t child = 0u; child < parent.childCount; ++child)
    {
      const auto &childNode = output.nodes[output.childIndices[parent.firstChild + child]];
      monotonic &= parent.selfError >= childNode.selfError && childNode.parentError == parent.selfError;
      childMaximumError = std::max(childMaximumError, childNode.selfError);
    }
    const float floorError = parent.minimumVoxelSize[3] * svr::kSparseVoxelBrickErrorFloor;
    measuredRepresentativeError |= parent.selfError > std::max(childMaximumError, floorError) + 1.0e-5f;
  }
  check(monotonic, "parent errors are monotonic and links are octant-bounded");
  check(measuredRepresentativeError, "at least one terrain parent error is measured from hidden representative surface deviation, not only voxel-size floor");
  svr::TerrainGPUV2RasterHierarchyOutput occupancyOnlyErrorOutput{};
  std::string error;
  check(svr::buildTerrainGPUV2RasterHierarchyOracle(source.config, source.raster.result, source.raster.nodes, source.raster.occupancy, source.raster.materialWords, occupancyOnlyErrorOutput, &error),
      "Phase 6 occupancy-only fallback oracle still builds: " + error);
  const auto smoothSource = fixture(0u, 17u, true);
  const auto smoothOutput = build(smoothSource);
  svr::TerrainGPUV2RasterHierarchyOutput smoothOccupancyOnly{};
  check(svr::buildTerrainGPUV2RasterHierarchyOracle(
            smoothSource.config,
            smoothSource.raster.result,
            smoothSource.raster.nodes,
            smoothSource.raster.occupancy,
            smoothSource.raster.materialWords,
            smoothOccupancyOnly,
            &error),
      "Phase 6 flat occupancy-only fallback oracle still builds: " + error);
  bool continuousHeightRaisedError = false;
  for (uint32_t parentIndex = smoothOutput.result.leafNodeCount;
       parentIndex < smoothOutput.nodes.size() &&
       parentIndex < smoothOccupancyOnly.nodes.size();
       ++parentIndex)
    continuousHeightRaisedError |=
        smoothOutput.nodes[parentIndex].selfError >
        smoothOccupancyOnly.nodes[parentIndex].selfError + 1.0e-5f;
  check(continuousHeightRaisedError,
      "continuous pre-voxel height residuals raise parent error beyond the occupancy-derived oracle");

  const std::vector<float> thresholds{-1.0f, 0.25f, 1.0f, 1000.0f};
  bool validCuts = true;
  for (const float threshold : thresholds)
  {
    const auto cut = svr::selectTerrainGPUV2RasterHierarchyCut(output, threshold);
    const std::set<uint32_t> selected(cut.begin(), cut.end());
    for (uint32_t leaf = 0u; leaf < output.result.leafNodeCount; ++leaf)
    {
      uint32_t selectedOnPath = 0u;
      uint32_t walk = leaf;
      while (walk != svr::kSparseVoxelBrickInvalidIndex)
      {
        selectedOnPath += selected.count(walk) != 0u ? 1u : 0u;
        walk = output.nodes[walk].parentIndex;
      }
      validCuts &= selectedOnPath == 1u;
    }
    for (const uint32_t node : cut)
      validCuts &= output.nodes[node].parentIndex == svr::kSparseVoxelBrickInvalidIndex || selected.count(output.nodes[node].parentIndex) == 0u;
  }
  check(validCuts, "every LOD threshold selects exactly one node per path with no parent/child overlap");

  const auto second = build(source);
  check(output.nodes.size() == second.nodes.size() && std::memcmp(output.nodes.data(), second.nodes.data(), output.nodes.size() * sizeof(output.nodes[0])) == 0 &&
            output.childIndices == second.childIndices && output.rootIndices == second.rootIndices,
      "level scans, parent links, errors, and materials are deterministic");
}

void independentCoarseAndFailure()
{
  const auto coarse = fixture(1u, 29u);
  const auto output = build(coarse);
  check(output.result.terrainGeneration == 29u && coarse.config.canonicalStep == 2u && output.nodes.back().canonicalGridOriginScale[3] > 2,
      "a coarse spatial page builds its own hierarchy without fine-page payload or readback");
  bool exactBoundary = true;
  const float chunkMaximumX = coarse.config.chunkMinimumWorldX + coarse.config.bricksX * 8u * coarse.config.voxelSizeMetres;
  for (const uint32_t root : output.rootIndices)
  {
    const auto &node = output.nodes[root];
    const uint32_t occupiedMaximumX = (node.packedOccupiedBounds >> 9u) & 7u;
    exactBoundary &= node.minimumVoxelSize[0] + (occupiedMaximumX + 1u) * node.minimumVoxelSize[3] <= chunkMaximumX;
  }
  check(exactBoundary, "coarse root occupied bounds never cross the chunk's half-open maximum");

  auto stale = coarse.raster.result;
  ++stale.terrainGeneration;
  svr::TerrainGPUV2RasterHierarchyOutput rejected{};
  std::string error;
  check(svr::buildTerrainGPUV2RasterHierarchyOracle(coarse.config, stale, coarse.raster.nodes, coarse.raster.occupancy, coarse.raster.materialWords, rejected, &error) &&
            rejected.result.status == static_cast<uint32_t>(svr::TerrainGPUV2RasterHierarchyStatus::StaleRaster) && rejected.nodes.empty(),
      "stale fine-raster generations cannot construct hierarchy output");
}
} // namespace

int main()
{
  std::cout << "Sparse voxel terrain GPU V2 Phase 6 tests\n";
  abiBudgetAndQualification();
  hierarchyAndCuts();
  independentCoarseAndFailure();
  if (failures != 0)
    std::cerr << failures << " Phase 6 checks failed\n";
  return failures == 0 ? 0 : 1;
}
