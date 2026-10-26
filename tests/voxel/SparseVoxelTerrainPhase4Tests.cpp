#include "TerrainGenerationPhase0Utils.hpp"
#include "voxel/svraster/TerrainGPUTree64Build.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <string>

#undef NDEBUG
#include <cassert>

namespace
{
namespace svr = voxel::svraster;

svr::WorldMapTerrainColumnPage pageForRequiredSide(
    const uint32_t requiredCells)
{
  const uint32_t columnSide = std::min(8u, requiredCells);
  svr::WorldMapTerrainColumnPage page{};
  page.side = columnSide;
  page.maximumVerticalVoxels = requiredCells;
  page.heightUnitMetres = 1.0 /
      svr::kWorldMapTerrainHeightUnitsPerVoxel;
  page.voxelSizeMetres = 0.5;
  page.edgeMetres = page.voxelSizeMetres * columnSide;
  page.columns.resize(uint64_t{columnSide} * columnSide);
  page.walls.resize(page.columns.size());
  page.canonicalCornerHeights.resize(
      uint64_t{columnSide + 1u} * (columnSide + 1u));
  page.canonicalBoundaryMinimumHeights.resize(uint64_t{4u} * columnSide);
  page.canonicalBoundaryMaximumHeights.resize(uint64_t{4u} * columnSide);
  for (uint32_t z = 0u; z < columnSide; ++z)
    for (uint32_t x = 0u; x < columnSide; ++x)
    {
      const uint32_t top = x == 0u && z == 0u
          ? requiredCells - 1u
          : std::min(requiredCells - 1u, 1u + x * 3u + z * 5u);
      const uint32_t first = top > 7u ? top - 7u : 0u;
      const uint32_t index = z * columnSide + x;
      page.columns[index] = {
        top, first,
        static_cast<uint32_t>(svr::SparseVoxelMicroContext::FlatSurface)
            << 8u,
        0u,
      };
      page.walls[index] = {
        first, top == 0u ? 0u : top - 1u,
        static_cast<uint32_t>(svr::SparseVoxelMicroContext::WallPositiveX) |
            (static_cast<uint32_t>(
                svr::SparseVoxelMicroContext::WallPositiveZ) << 8u),
        0u,
      };
    }
  return page;
}

uint32_t maskCapacity(const svr::WorldMapTerrainColumnPage &page)
{
  uint32_t required = page.side;
  for (const auto &column : page.columns)
    required = std::max(required, column.topY + 1u);
  uint64_t total = 0u;
  uint32_t horizontal = (page.side + 3u) / 4u;
  uint32_t vertical = (required + 3u) / 4u;
  for (;;)
  {
    total += uint64_t{horizontal} * vertical * horizontal;
    if (horizontal == 1u && vertical == 1u) break;
    horizontal = (horizontal + 3u) / 4u;
    vertical = (vertical + 3u) / 4u;
  }
  assert(total <= UINT32_MAX);
  return static_cast<uint32_t>(total);
}

svr::SparseVoxelTerrainPageTree64 reference(
    const svr::WorldMapTerrainColumnPage &page)
{
  svr::SparseVoxelTerrainPageTree64 tree{};
  std::string error;
  assert(svr::buildSparseVoxelTerrainPageTree64FromColumns(
      page, tree, &error));
  return tree;
}

void assertColumnOccupancy(const svr::WorldMapTerrainColumnPage &page,
    const svr::SparseVoxelTree64Chunk &tree)
{
  for (uint32_t z = 0u; z < page.side; ++z)
    for (uint32_t x = 0u; x < page.side; ++x)
    {
      const auto &column = page.columns[z * page.side + x];
      for (uint32_t y = 0u; y < tree.side; ++y)
        assert(svr::sparseVoxelTree64Occupied(tree, {x, y, z}) ==
            (y >= column.firstSolidY && y <= column.topY));
    }
  std::mt19937 random(0x4d595df4u);
  for (uint32_t sample = 0u; sample < 2048u; ++sample)
  {
    const std::array<uint32_t, 3> cell{
      random() % tree.side, random() % tree.side, random() % tree.side};
    bool expected = false;
    if (cell[0] < page.side && cell[2] < page.side)
    {
      const auto &column = page.columns[cell[2] * page.side + cell[0]];
      expected = cell[1] >= column.firstSolidY && cell[1] <= column.topY;
    }
    assert(svr::sparseVoxelTree64Occupied(tree, cell) == expected);
  }
}

void testSupportedSidesAndPlanner()
{
  for (const uint32_t required : {4u, 16u, 64u, 256u, 1024u})
  {
    const auto page = pageForRequiredSide(required);
    const auto expected = reference(page);
    assert(expected.side == required);
    std::string error;
    assert(svr::validateTerrainGPUTree64Chunk(expected.tree, &error));
    assertColumnOccupancy(page, expected.tree);
    const svr::TerrainGPUTree64Capacity capacity{
      maskCapacity(page), static_cast<uint32_t>(expected.tree.nodes.size())};
    svr::TerrainGPUTree64ConfigGPU config{};
    assert(svr::planTerrainGPUTree64Build(page, capacity, 19u,
        0x123456789abcdef0ull, 7u, 11u, 13u, config, &error));
    svr::TerrainGPUTree64ConfigGPU metadataConfig{};
    assert(svr::planTerrainGPUTree64Build(
        svr::TerrainGPUTree64PageDescription{
          page.side, required, page.internalLOD,
          page.voxelSizeMetres, page.edgeMetres},
        capacity, 19u, 0x123456789abcdef0ull, 7u, 11u, 13u,
        metadataConfig, &error));
    const uint32_t expectedOccupied = config.occupiedVoxelCount;
    config.occupiedVoxelCount = 0u;
    assert(std::memcmp(&config, &metadataConfig, sizeof(config)) == 0);
    config.occupiedVoxelCount = expectedOccupied;
    assert(config.side == required);
    assert(config.levelCounts[config.levelCount - 1u] == 1u);
    assert(config.expectedTerrainGeneration == 19u);
    assert(svr::terrainGPUTree64SemanticHash(expected.tree) ==
        svr::terrainGPUTree64SemanticHash(expected.tree));
    auto tooSmall = capacity;
    --tooSmall.masks;
    assert(!svr::planTerrainGPUTree64Build(page, tooSmall, 1u, 1u,
        1u, 1u, 1u, config, &error));
  }
}

void testPhase0FixtureMatrix()
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
      const auto expected = reference(columns);
      assert(svr::validateTerrainGPUTree64Chunk(expected.tree, &error));
      const svr::TerrainGPUTree64Capacity capacity{
        maskCapacity(columns),
        static_cast<uint32_t>(expected.tree.nodes.size()),
      };
      svr::TerrainGPUTree64ConfigGPU config{};
      assert(svr::planTerrainGPUTree64Build(columns, capacity, lod + 1u,
          svr::kTerrainGPUTree64SemanticRevision, 3u, 5u, 7u,
          config, &error));
    }
}
} // namespace

int main()
{
  testSupportedSidesAndPlanner();
  testPhase0FixtureMatrix();
  std::cout << "GPU terrain Phase 4 Tree64 contract tests passed\n";
  return 0;
}
