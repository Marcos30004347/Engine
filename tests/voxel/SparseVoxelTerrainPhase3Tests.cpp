#include "voxel/svraster/TerrainGPURasterBuild.hpp"
#include "TerrainGenerationPhase0Utils.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

#undef NDEBUG
#include <cassert>

namespace
{
namespace svr = voxel::svraster;

svr::WorldMapTerrainColumnPage propertyPage(const uint32_t side = 8u,
    const uint32_t height = 32u)
{
  svr::WorldMapTerrainColumnPage page{};
  page.side = side;
  page.maximumVerticalVoxels = height;
  page.heightUnitMetres = 1.0 /
      svr::kWorldMapTerrainHeightUnitsPerVoxel;
  page.voxelSizeMetres = 1.0;
  page.edgeMetres = side;
  page.columns.resize(uint64_t{side} * side);
  page.walls.resize(page.columns.size());
  page.canonicalCornerHeights.resize(uint64_t{side + 1u} * (side + 1u));
  page.canonicalBoundaryMinimumHeights.resize(uint64_t{4u} * side);
  page.canonicalBoundaryMaximumHeights.resize(uint64_t{4u} * side);
  for (uint32_t z = 0u; z < side; ++z)
    for (uint32_t x = 0u; x < side; ++x)
    {
      const uint32_t index = z * side + x;
      const uint32_t top = std::min(height - 1u,
          1u + ((x * 7u + z * 11u) % (height - 1u)));
      const uint32_t depth = 1u + ((x + z * 3u) % std::min(8u, top + 1u));
      const uint32_t first = top + 1u - depth;
      const uint32_t unstitched = std::min(top, first + (x == 0u ? 2u : 0u));
      page.columns[index] = {
        top, first,
        ((x + z) % 5u) |
            (static_cast<uint32_t>(svr::SparseVoxelMicroContext::FlatSurface)
                << 8u),
        unstitched != first
            ? svr::WorldMapTerrainColumnBoundaryStitched : 0u,
      };
      page.walls[index] = {
        unstitched, top > 2u ? top - 2u : 0u,
        static_cast<uint32_t>(svr::SparseVoxelMicroContext::WallPositiveX) |
            (static_cast<uint32_t>(
                svr::SparseVoxelMicroContext::WallNegativeZ) << 8u),
        0u,
      };
    }
  return page;
}

svr::SparseVoxelBrickHierarchy reference(
    const svr::WorldMapTerrainColumnPage &page)
{
  svr::WorldMapTerrainColumnLeafPayload payload{};
  std::string error;
  assert(svr::buildWorldMapTerrainColumnLeafPayload(page, payload, &error));
  svr::SparseVoxelBrickBuildSettings settings{};
  settings.voxelSize = static_cast<float>(page.voxelSizeMetres);
  svr::SparseVoxelBrickHierarchy hierarchy{};
  assert(svr::buildSparseVoxelBrickHierarchy(payload.leaves,
      payload.materials, payload.indices, settings, hierarchy, &error));
  return hierarchy;
}

void testPlanAndMaterialTable()
{
  const auto page = propertyPage();
  const auto hierarchy = reference(page);
  const svr::TerrainGPURasterCapacity capacity{
    64u,
    static_cast<uint32_t>(hierarchy.nodes.size()),
    static_cast<uint32_t>(hierarchy.childIndices.size()),
    static_cast<uint32_t>(hierarchy.materialIndices.size()),
    static_cast<uint32_t>(hierarchy.rootIndices.size()),
  };
  svr::TerrainGPURasterConfigGPU config{};
  std::string error;
  assert(svr::planTerrainGPURasterBuild(
      page, capacity, 17u, 0x123456789abcdef0ull,
      static_cast<uint32_t>(hierarchy.palette.size()), config, &error));
  assert(config.side == page.side && config.maximumLevelCount >= 2u);
  assert(config.nodeCapacity == hierarchy.nodes.size());
  assert(config.expectedTerrainGeneration == 17u);
  assert(config.buildRevisionLow == 0x9abcdef0u);
  assert(config.buildRevisionHigh == 0x12345678u);
  svr::TerrainGPURasterConfigGPU metadataConfig{};
  assert(svr::planTerrainGPURasterBuild(
      svr::TerrainGPURasterPageDescription{
        page.side, page.maximumVerticalVoxels,
        page.voxelSizeMetres, page.edgeMetres},
      capacity, 17u, 0x123456789abcdef0ull,
      static_cast<uint32_t>(hierarchy.palette.size()), metadataConfig,
      &error));
  assert(std::memcmp(&config, &metadataConfig, sizeof(config)) == 0);

  std::array<svr::TerrainGPURasterMaterialGPU,
      svr::kTerrainGPURasterMaterialTableSize> table{};
  assert(svr::buildTerrainGPURasterMaterialTable(
      page, hierarchy, table, &error));
  std::array<svr::TerrainGPURasterMaterialGPU,
      svr::kTerrainGPURasterMaterialTableSize> metadataTable{};
  assert(svr::buildTerrainGPURasterMaterialTable(
      hierarchy, metadataTable, &error));
  for (const auto &column : page.columns)
  {
    assert(table[svr::worldMapTerrainColumnMaterial(column)].paletteEntry !=
        svr::kSparseVoxelBrickInvalidIndex);
    const uint32_t material = svr::worldMapTerrainColumnMaterial(column);
    assert(std::memcmp(&table[material], &metadataTable[material],
        sizeof(table[material])) == 0);
  }

  auto tooSmall = capacity;
  tooSmall.denseCandidates = 1u;
  assert(!svr::planTerrainGPURasterBuild(page, tooSmall, 1u, 1u,
      static_cast<uint32_t>(hierarchy.palette.size()), config, &error));
}

void testIndependentValidator()
{
  const auto valid = reference(propertyPage());
  std::string error;
  assert(svr::validateTerrainGPURasterHierarchy(valid, &error));
  assert(svr::terrainGPURasterSemanticHash(valid) ==
      svr::terrainGPURasterSemanticHash(valid));

  auto badPrefix = valid;
  badPrefix.occupancy.front().cells[1].prefix += 1u;
  assert(!svr::validateTerrainGPURasterHierarchy(badPrefix, &error));

  auto badLink = valid;
  const auto parent = std::find_if(badLink.nodes.begin(), badLink.nodes.end(),
      [](const auto &node) { return node.childCount != 0u; });
  assert(parent != badLink.nodes.end());
  const uint32_t parentIndex = static_cast<uint32_t>(
      std::distance(badLink.nodes.begin(), parent));
  const uint32_t child = badLink.childIndices[parent->firstChild];
  badLink.nodes[child].parentIndex = parentIndex + 1u;
  assert(!svr::validateTerrainGPURasterHierarchy(badLink, &error));

  auto badError = valid;
  const auto errorParent = std::find_if(badError.nodes.begin(),
      badError.nodes.end(), [](const auto &node) { return node.childCount != 0u; });
  assert(errorParent != badError.nodes.end());
  badError.nodes[badError.childIndices[errorParent->firstChild]].selfError =
      errorParent->selfError + 1.0f;
  assert(!svr::validateTerrainGPURasterHierarchy(badError, &error));
}

void testPhase0FixtureMatrixPlans()
{
  for (const auto kind : terrain_phase0::kFixtureKinds)
    for (uint32_t lod = 0u; lod <= 2u; ++lod)
    {
      const auto bundle = terrain_phase0::makeFixture(kind);
      const auto settings = terrain_phase0::fixtureSettings(kind, lod);
      svr::PlanetVoxelChunkSet chunks{};
      svr::WorldMapTerrainColumnPage columns{};
      std::string error;
      assert(svr::buildWorldMapVoxelTerrain(bundle, settings, chunks, &error,
          {}, nullptr, nullptr, &columns));
      const auto hierarchy = reference(columns);
      assert(svr::validateTerrainGPURasterHierarchy(hierarchy, &error));
      const svr::TerrainGPURasterCapacity capacity{
        4096u,
        static_cast<uint32_t>(hierarchy.nodes.size()),
        std::max(1u, static_cast<uint32_t>(hierarchy.childIndices.size())),
        static_cast<uint32_t>(hierarchy.materialIndices.size()),
        static_cast<uint32_t>(hierarchy.rootIndices.size()),
      };
      svr::TerrainGPURasterConfigGPU config{};
      if (!svr::planTerrainGPURasterBuild(columns, capacity, lod + 1u,
          svr::kTerrainGPURasterSemanticRevision,
          static_cast<uint32_t>(hierarchy.palette.size()), config, &error))
      {
        std::cerr << "Phase 3 plan failed for "
                  << terrain_phase0::fixtureName(kind) << " LOD " << lod
                  << ": " << error << '\n';
        assert(false);
      }
      std::array<svr::TerrainGPURasterMaterialGPU,
          svr::kTerrainGPURasterMaterialTableSize> table{};
      assert(svr::buildTerrainGPURasterMaterialTable(
          columns, hierarchy, table, &error));
    }
}
} // namespace

int main()
{
  testPlanAndMaterialTable();
  testIndependentValidator();
  testPhase0FixtureMatrixPlans();
  std::cout << "GPU terrain Phase 3 raster contract tests passed\n";
  return 0;
}
