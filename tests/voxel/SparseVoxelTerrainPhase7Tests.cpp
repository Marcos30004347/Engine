#include "procedural/world/MapExport.hpp"
#include "procedural/world/MapExportGpuCache.hpp"
#include "voxel/svraster/TerrainGPUColumnBuild.hpp"
#include "voxel/svraster/rendering/TerrainGPUColumnBuildChain.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

#undef NDEBUG
#include <cassert>

namespace
{
namespace world = procedural::world;
namespace svr = voxel::svraster;

world::MapExportField field(const char *name, const world::MapExportFormat format, const world::MapExportFilter filter, const char *group = "")
{
  world::MapExportField result{};
  result.name = name;
  result.format = format;
  result.filter = filter;
  result.available = true;
  result.width = 4u;
  result.height = 4u;
  result.samplingGroup = group;
  return result;
}

world::MapExportBundle bundle()
{
  world::MapExportBundle result{};
  result.resolution = 4u;
  result.pageSizePixels = 2u;
  result.gutterPixels = 1u;
  result.maximumPageLevel = 1u;
  result.extentMetres = 16.0;
  result.seed = 0x123456789abcdef0ull;
  result.bundleSignature = 0xfedcba9876543210ull;
  result.fields = {
    field("refined_elevation", world::MapExportFormat::R16Unorm, world::MapExportFilter::Linear),
    field("base_elevation", world::MapExportFormat::R16Unorm, world::MapExportFilter::Linear),
    field("water_type", world::MapExportFormat::R8Uint, world::MapExportFilter::Nearest),
    field("water_level", world::MapExportFormat::R16Float, world::MapExportFilter::Linear),
    field("river", world::MapExportFormat::RG16Float, world::MapExportFilter::Linear),
    field("substrate_id", world::MapExportFormat::R8Uint, world::MapExportFilter::Nearest),
    field("soil_depth", world::MapExportFormat::R16Float, world::MapExportFilter::Linear),
    field("biome_mix_ids", world::MapExportFormat::RGBA8Uint, world::MapExportFilter::SparseMixture, "biome"),
    field("biome_mix_weights", world::MapExportFormat::RGBA8Unorm, world::MapExportFilter::SparseMixture, "biome"),
    field("relief_roughness", world::MapExportFormat::RG16Float, world::MapExportFilter::Linear),
    field("landform_id", world::MapExportFormat::R8Uint, world::MapExportFilter::Nearest),
    field("climate", world::MapExportFormat::RG16Float, world::MapExportFilter::Linear),
    field("roads", world::MapExportFormat::R8Uint, world::MapExportFilter::Nearest),
  };
  return result;
}

svr::TerrainGPUColumnConfigGPU config(const world::MapExportBundle &source)
{
  world::MapExportGpuPageLayout layout;
  std::string error;
  assert(layout.initialize(source, 64u, &error));
  svr::TerrainGPUColumnBuildSettings settings{};
  settings.normalizedBounds = {0.0, 0.0, 1.0, 1.0};
  settings.side = 8u;
  settings.halo = 2u;
  settings.maximumVerticalVoxels = 4096u;
  settings.requestedLevel = 1u;
  settings.boundaryStitchSubdivisions = 2u;
  settings.surfaceShellDepthVoxels = 2u;
  settings.finestVoxelSizeMetres = 2.0;
  settings.elevationOriginMetres = -100.0;
  settings.expectedTerrainGeneration = 17u;
  settings.expectedAtlasRevision = 29u;
  svr::TerrainGPUColumnConfigGPU result{};
  assert(svr::planTerrainGPUColumnBuild(source, layout, settings, result, &error));
  assert(result.generatorRevisionLow == svr::kTerrainGPUColumnGeneratorRevision);
  return result;
}

svr::TerrainGPUColumnSurfaceSamples samples(const svr::TerrainGPUColumnConfigGPU &config, const float xOffset)
{
  svr::TerrainGPUColumnSurfaceSamples result{};
  const uint32_t side = config.side + 2u * config.halo + 1u;
  result.cornerHeights.resize(static_cast<size_t>(side) * side);
  for (uint32_t z = 0u; z < side; ++z)
    for (uint32_t x = 0u; x < side; ++x)
    {
      const float worldX = xOffset + static_cast<float>(static_cast<int32_t>(x) - static_cast<int32_t>(config.halo)) * config.voxelSizeMetres;
      const float worldZ = static_cast<float>(static_cast<int32_t>(z) - static_cast<int32_t>(config.halo)) * config.voxelSizeMetres;
      const float cliff = worldX >= 8.0f ? 24.0f : 0.0f;
      result.cornerHeights[static_cast<size_t>(z) * side + x] = 320.0f + cliff + worldZ * 0.125f;
    }
  result.materials.resize(static_cast<size_t>(config.side) * config.side);
  for (uint32_t z = 0u; z < config.side; ++z)
    for (uint32_t x = 0u; x < config.side; ++x)
      result.materials[static_cast<size_t>(z) * config.side + x] = (x + z) % 7u;
  return result;
}

void testPlannerAndCanonicalShell()
{
  const auto source = bundle();
  auto buildConfig = config(source);
  const auto input = samples(buildConfig, 0.0f);
  svr::WorldMapTerrainColumnPage first{};
  svr::WorldMapTerrainColumnPage second{};
  std::string error;
  assert(svr::buildTerrainGPUColumnOracle(buildConfig, input, first, &error));
  assert(svr::buildTerrainGPUColumnOracle(buildConfig, input, second, &error));
  assert(svr::terrainGPUColumnSemanticHash(first) == svr::terrainGPUColumnSemanticHash(second));
  assert(first.elevationOriginMetres == -100.0);
  assert(first.columns.size() == 64u && first.walls.size() == 64u);
  bool sawCliffWall = false;
  for (size_t index = 0u; index < first.columns.size(); ++index)
  {
    assert(first.columns[index].topY < buildConfig.maximumVerticalVoxels);
    assert(first.columns[index].firstSolidY <= first.columns[index].topY);
    sawCliffWall = sawCliffWall || first.walls[index].lowestNeighbourY < first.columns[index].topY;
  }
  assert(sawCliffWall);
}

void testSharedEdgeAndMountainCapacity()
{
  const auto source = bundle();
  auto leftConfig = config(source);
  auto rightConfig = leftConfig;
  const auto leftSamples = samples(leftConfig, 0.0f);
  const auto rightSamples = samples(rightConfig, 16.0f);
  svr::WorldMapTerrainColumnPage left{};
  svr::WorldMapTerrainColumnPage right{};
  std::string error;
  assert(svr::buildTerrainGPUColumnOracle(leftConfig, leftSamples, left, &error));
  assert(svr::buildTerrainGPUColumnOracle(rightConfig, rightSamples, right, &error));
  // A separately sampled page that starts at left's world edge quantizes that
  // edge against the same global datum; there is no per-page Y renumbering.
  for (uint32_t z = 0u; z <= leftConfig.side; ++z)
    assert(left.canonicalCornerHeights[static_cast<size_t>(z) * (leftConfig.side + 1u) + leftConfig.side] == right.canonicalCornerHeights[static_cast<size_t>(z) * (rightConfig.side + 1u)]);

  auto mountain = samples(leftConfig, 0.0f);
  std::fill(mountain.cornerHeights.begin(), mountain.cornerHeights.end(), 4500.0f);
  svr::WorldMapTerrainColumnPage high{};
  // Two-metre voxels provide 8.2 km of capacity; the 4.5 km adversarial
  // summit remains representable with the common -100 m datum.
  assert(svr::buildTerrainGPUColumnOracle(leftConfig, mountain, high, &error));
  assert(high.columns.front().topY > 2200u);
}

void testFailureContracts()
{
  auto source = bundle();
  world::MapExportGpuPageLayout layout;
  std::string error;
  assert(layout.initialize(source, 64u, &error));
  svr::TerrainGPUColumnBuildSettings settings{};
  settings.side = 8u;
  settings.maximumVerticalVoxels = 4096u;
  settings.finestVoxelSizeMetres = 2.0;
  settings.elevationOriginMetres = -100.0;
  settings.boundaryStitchSubdivisions = 0u;
  svr::TerrainGPUColumnConfigGPU output{};
  assert(!svr::planTerrainGPUColumnBuild(source, layout, settings, output, &error));

  auto valid = config(source);
  auto invalidSamples = samples(valid, 0.0f);
  invalidSamples.cornerHeights.pop_back();
  svr::WorldMapTerrainColumnPage page{};
  assert(!svr::buildTerrainGPUColumnOracle(valid, invalidSamples, page, &error));
}
} // namespace

int main()
{
  testPlannerAndCanonicalShell();
  testSharedEdgeAndMountainCapacity();
  testFailureContracts();
  std::cout << "Sparse voxel terrain Phase 7 tests passed\n";
  return 0;
}
