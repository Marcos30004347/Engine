#include "TerrainGenerationPhase0Utils.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
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

bool buildFixture(const terrain_phase0::FixtureKind kind, const uint32_t lod, svr::PlanetVoxelChunkSet &page, svr::WorldMapVoxelGenerationMetrics &metrics, std::string &error)
{
  const auto bundle = terrain_phase0::makeFixture(kind);
  const auto settings = terrain_phase0::fixtureSettings(kind, lod);
  error.clear();
  return svr::buildWorldMapVoxelTerrain(bundle, settings, page, &error, {}, nullptr, &metrics);
}

bool coarseSurfaceContainsFine(const svr::PlanetVoxelChunkSet &fine, const svr::PlanetVoxelChunkSet &coarse)
{
  if (fine.chunks.empty() || coarse.chunks.empty() || fine.surfaceHeightSide == 0u || coarse.surfaceHeightSide == 0u)
    return false;
  const uint32_t fineSide = fine.surfaceHeightSide - fine.surfaceHeightHalo * 2u;
  const uint32_t coarseSide = coarse.surfaceHeightSide - coarse.surfaceHeightHalo * 2u;
  if (coarseSide == 0u || fineSide % coarseSide != 0u)
    return false;
  const uint32_t scale = fineSide / coarseSide;
  for (uint32_t z = 0u; z < fineSide; ++z)
    for (uint32_t x = 0u; x < fineSide; ++x)
    {
      const size_t fineIndex = static_cast<size_t>(z + fine.surfaceHeightHalo) * fine.surfaceHeightSide + x + fine.surfaceHeightHalo;
      const size_t coarseIndex = static_cast<size_t>(z / scale + coarse.surfaceHeightHalo) * coarse.surfaceHeightSide + x / scale + coarse.surfaceHeightHalo;
      const double coarseEnvelope = coarse.surfaceEnvelope[coarseIndex];
      if (coarseEnvelope + 1.0e-4 < fine.surfaceHeights[fineIndex])
        return false;
    }
  return true;
}

} // namespace

int main()
{
  std::cout << "GPU terrain generation Phase 0 CPU baselines\n";
  std::set<uint64_t> finestFixtureHashes;
  std::vector<svr::PlanetVoxelChunkSet> crossLODPages;

  for (const auto kind : terrain_phase0::kFixtureKinds)
  {
    std::cout << terrain_phase0::fixtureName(kind) << '\n';
    for (uint32_t lod = 0u; lod < 3u; ++lod)
    {
      svr::PlanetVoxelChunkSet first{};
      svr::WorldMapVoxelGenerationMetrics firstMetrics{};
      std::string error;
      const bool firstBuilt = buildFixture(kind, lod, first, firstMetrics, error);
      check(firstBuilt, std::string("LOD ") + std::to_string(lod) + " builds: " + error);
      if (!firstBuilt)
        continue;

      svr::PlanetVoxelChunkSet repeated{};
      svr::WorldMapVoxelGenerationMetrics repeatedMetrics{};
      const bool repeatedBuilt = buildFixture(kind, lod, repeated, repeatedMetrics, error);
      const auto firstBytes = svr::serializeWorldVoxelTransientPage(first);
      const auto repeatedBytes = svr::serializeWorldVoxelTransientPage(repeated);
      check(repeatedBuilt && firstBytes == repeatedBytes, std::string("LOD ") + std::to_string(lod) + " rebuild is byte-identical");

      terrain_phase0::SemanticTerrainPage firstSemantic{};
      terrain_phase0::SemanticTerrainPage repeatedSemantic{};
      std::string difference;
      const bool decoded = terrain_phase0::decodeSemanticTerrainPage(first, 0u, firstSemantic, &difference);
      const bool repeatedDecoded = terrain_phase0::decodeSemanticTerrainPage(repeated, 0u, repeatedSemantic, &difference);
      const bool semanticEqual = decoded && repeatedDecoded && terrain_phase0::compareSemanticTerrainPages(firstSemantic, repeatedSemantic, &difference);
      check(semanticEqual, std::string("LOD ") + std::to_string(lod) + " semantic payload agrees: " + difference);

      svr::SparseVoxelTerrainPageTree64 tree64{};
      const bool occupancyEqual = terrain_phase0::compareRasterAndTree64Occupancy(first, 0u, &tree64, &difference);
      check(occupancyEqual, std::string("LOD ") + std::to_string(lod) + " raster and Tree64 finest occupancy agree: " + difference);

      check(
          firstMetrics.mapSamplingMilliseconds >= 0.0 && firstMetrics.shellExtractionMilliseconds >= 0.0 && firstMetrics.hierarchyBuildMilliseconds >= 0.0 && firstMetrics.mapSampleCount != 0u &&
              firstMetrics.emittedShellVoxelCount == first.leafVoxelCount && firstMetrics.brickLevelCount == first.bricks.levelCount && firstMetrics.brickNodeCount == first.bricks.nodes.size() &&
              firstMetrics.horizontalVoxelCount != 0u && firstMetrics.verticalVoxelCapacity != 0u && terrain_phase0::semanticTerrainRunCount(firstSemantic) != 0u &&
              firstMetrics.brickRootCount == first.bricks.rootIndices.size() && firstMetrics.brickPayloadBytes != 0u && firstMetrics.surfacePayloadBytes != 0u && tree64.levelCount != 0u &&
              tree64.payloadBytes == tree64.tree.nodes.size() * sizeof(svr::SparseVoxelTree64NodeGPU) && tree64.buildMilliseconds >= 0.0,
          std::string("LOD ") + std::to_string(lod) + " reports complete phase, hierarchy, Tree64, and payload metrics");

      if (decoded && lod == 0u)
        finestFixtureHashes.insert(terrain_phase0::semanticTerrainPageHash(firstSemantic));
      if (kind == terrain_phase0::FixtureKind::CrossLOD)
        crossLODPages.push_back(std::move(first));
    }
  }

  check(finestFixtureHashes.size() == terrain_phase0::kFixtureKinds.size(), "every named finest-LOD fixture has distinct semantic content");
  check(crossLODPages.size() == 3u && coarseSurfaceContainsFine(crossLODPages[0], crossLODPages[2]), "cross-LOD fixture retains conservative coarse surface coverage");

  if (!crossLODPages.empty())
  {
    terrain_phase0::SemanticTerrainPage expected{};
    std::string difference;
    const bool decoded = terrain_phase0::decodeSemanticTerrainPage(crossLODPages.front(), 0u, expected, &difference);
    auto changed = expected;
    if (!changed.cells.empty())
      changed.cells.front().context ^= 1u;
    const bool detected = decoded && !terrain_phase0::compareSemanticTerrainPages(expected, changed, &difference) && difference.find("context differs at canonical cell") != std::string::npos;
    check(detected, "semantic differential reports the first changed canonical cell");
  }

  return failures == 0 ? 0 : 1;
}
