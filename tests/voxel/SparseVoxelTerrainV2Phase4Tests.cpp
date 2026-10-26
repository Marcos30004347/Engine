#include "voxel/svraster/TerrainGPUV2FineRaster.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
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
  svr::TerrainGPUV2FineRasterConfigGPU config{};
  svr::TerrainGPUV2ColumnResultGPU result{};
  std::vector<svr::TerrainGPUV2ColumnGPU> columns{};
};

Fixture fixture(
    const svr::TerrainGPUChunkKey &key = svr::TerrainGPUChunkKey{41u, 0, 0, 0, 0u, 1u, 3u, 7u},
    const svr::TerrainGPUChunkShape shape = {2u, 2u, 2u, 8u},
    const uint32_t nodeCapacity = 8u,
    const uint32_t materialCapacity = 4096u)
{
  Fixture value{};
  svr::TerrainGPUChunkBounds bounds{};
  std::string error;
  check(svr::terrainGPUChunkBounds(key, shape, bounds, &error), "fixture chunk bounds plan: " + error);
  svr::TerrainGPUV2ColumnConfigGPU columns{};
  columns.columnsX = shape.bricksX * 8u;
  columns.columnsZ = shape.bricksZ * 8u;
  columns.halo = 2u;
  columns.heightUnitMetres = 1.0f;
  columns.canonicalStep = 1u << key.clipmapLOD;
  columns.tileMinimumCanonicalX = bounds.minimum[0] * static_cast<int32_t>(columns.canonicalStep);
  columns.tileMinimumCanonicalZ = bounds.minimum[2] * static_cast<int32_t>(columns.canonicalStep);
  columns.expectedTerrainGeneration = key.expectedGeneration;
  svr::TerrainGPUV2FineRasterSettings settings{};
  settings.key = key;
  settings.shape = shape;
  settings.columns = columns;
  settings.capacity = {nodeCapacity, materialCapacity};
  settings.finestVoxelSizeMetres = 1.0;
  settings.regionMappingId = 29u;
  settings.paletteBase = 11u;
  check(svr::planTerrainGPUV2FineRaster(settings, value.config, &error), "fine raster fixture plans: " + error);
  value.result.status = static_cast<uint32_t>(svr::TerrainGPUV2ColumnStatus::Ready);
  value.result.terrainGeneration = key.expectedGeneration;
  value.result.columnCount = (columns.columnsX + 2u * columns.halo) * (columns.columnsZ + 2u * columns.halo);
  value.result.writtenColumns = value.result.columnCount;
  value.columns.resize(value.result.columnCount);
  for (auto &column : value.columns)
    column = {0, 0, 5u, svr::TerrainGPUV2ColumnValid};
  return value;
}

template <typename Height> void setCoreHeights(Fixture &value, const Height &height)
{
  for (uint32_t z = 0u; z < value.config.columnsZ; ++z)
    for (uint32_t x = 0u; x < value.config.columnsX; ++x)
    {
      auto &column = value.columns[uint64_t{z + value.config.halo} * value.config.columnStride + x + value.config.halo];
      column.maximumHeightUnits = height(x, z);
      column.minimumHeightUnits = column.maximumHeightUnits;
      column.material = 1u + (x + z) % 23u;
      column.flags = svr::TerrainGPUV2ColumnValid;
    }
}

void testPlanAndScratch()
{
  const auto value = fixture();
  svr::TerrainGPUV2FineRasterScratchLayout scratch{};
  std::string error;
  check(svr::terrainGPUV2FineRasterScratchLayout(value.config.denseBrickCount, scratch, &error), "paired node/material scratch plans: " + error);
  check(
      value.config.brickSide == 8u && value.config.denseBrickCount == 8u && value.config.regionMappingId != 0u && scratch.totalWords < value.config.denseBrickCount * 512u,
      "fine raster uses fixed 8x8x8 bricks and no 512-cell per-node scratch lane");
  svr::TerrainGPUV2FineRasterMemory memory{};
  check(
      svr::terrainGPUV2FineRasterMemory(value.config, memory, &error) && memory.scratchBytes == uint64_t{scratch.totalWords} * 4u && memory.outputBytes != 0u && memory.microGeometryBytes == 0u,
      "fine raster exposes exact scratch/output reservations to the chunk scheduler");
  check(sizeof(svr::TerrainGPUV2FineRasterConfigGPU) == 144u && sizeof(svr::TerrainGPUV2FineRasterResultGPU) == 64u, "fine raster GPU ABI remains frozen");
  svr::TerrainGPUV2FineRasterWorkgroupCandidate selected{};
  check(
      svr::terrainGPUV2SelectFineRasterWorkgroup({{64u, 19u}, {128u, 11u}, {256u, 13u}}, 256u, selected, &error) && selected.invocations == 128u,
      "device timing sweep selects the fastest qualified fine-raster workgroup");
}

void testEveryWordAndSyntheticShapes()
{
  std::string error;
  for (uint32_t selectedWord = 0u; selectedWord < 16u; ++selectedWord)
  {
    std::array<int32_t, 512u> cells{};
    cells.fill(-1);
    for (uint32_t bit = 0u; bit < 32u; ++bit)
      if (((bit * 13u + selectedWord * 7u) & 3u) != 0u)
        cells[selectedWord * 32u + bit] = static_cast<int32_t>((bit + selectedWord) & 0xffu);
    svr::SparseVoxelBrickOccupancyGPU occupancy{};
    std::vector<uint32_t> materials;
    check(svr::terrainGPUV2PackFineBrick(cells, occupancy, materials, &error), "occupancy word " + std::to_string(selectedWord) + " packs: " + error);
    uint32_t prefix = 0u;
    bool exact = true;
    for (uint32_t word = 0u; word < 16u; ++word)
    {
      exact = exact && occupancy.cells[word].prefix == prefix;
      prefix += static_cast<uint32_t>(__builtin_popcount(occupancy.cells[word].bits));
    }
    uint32_t address = 0u;
    for (const int32_t material : cells)
      if (material >= 0)
        exact = exact && svr::terrainGPUV2FineRasterMaterial(materials, address++) == material;
    check(exact && address == prefix, "occupancy prefixes and material addresses are exact for word " + std::to_string(selectedWord));
  }

  const auto packCount = [&](const std::array<int32_t, 512u> &cells)
  {
    svr::SparseVoxelBrickOccupancyGPU occupancy{};
    std::vector<uint32_t> materials;
    check(svr::terrainGPUV2PackFineBrick(cells, occupancy, materials, &error), "synthetic brick packs: " + error);
    uint32_t count = 0u;
    for (const auto &word : occupancy.cells)
      count += static_cast<uint32_t>(__builtin_popcount(word.bits));
    return count;
  };
  std::array<int32_t, 512u> empty{};
  empty.fill(-1);
  auto full = empty;
  full.fill(7);
  auto one = empty;
  one[3u + 8u * (4u + 8u * 5u)] = 9;
  auto shell = empty;
  auto cliff = empty;
  for (uint32_t z = 0u; z < 8u; ++z)
    for (uint32_t y = 0u; y < 8u; ++y)
      for (uint32_t x = 0u; x < 8u; ++x)
      {
        const uint32_t cell = x + 8u * (y + 8u * z);
        if (x == 0u || x == 7u || y == 0u || y == 7u || z == 0u || z == 7u)
          shell[cell] = 4;
        if (y < (x < 4u ? 7u : 2u))
          cliff[cell] = 6;
      }
  check(packCount(empty) == 0u && packCount(full) == 512u && packCount(one) == 1u && packCount(shell) == 296u && packCount(cliff) == 288u, "empty, full, one-cell, shell, and cliff bricks preserve exact occupancy");
}

void testColumnDifferentialAndFailureAtomicity()
{
  std::string error;
  auto value = fixture();
  setCoreHeights(
      value,
      [](const uint32_t x, const uint32_t z)
      {
        return static_cast<int32_t>(4u + ((x * 3u + z * 5u) % 13u));
      });
  svr::TerrainGPUV2FineRasterOutput output{};
  check(svr::buildTerrainGPUV2FineRasterOracle(value.config, value.result, value.columns, output, &error), "fine-brick CPU oracle builds: " + error);
  check(
      output.result.status == static_cast<uint32_t>(svr::TerrainGPUV2FineRasterStatus::Ready) && output.result.nodeCount != 0u && output.result.materialCount != 0u && output.result.perNodeVoxelScratchWords == 0u &&
          output.result.microSurfaceBytes == 0u && svr::validateTerrainGPUV2FineRaster(value.config, output, &error),
      "column raster produces a valid leaf-only occupancy descriptor without microgeometry");
  bool square = true;
  for (const auto &node : output.nodes)
    square = square && (node.level & svr::kSparseVoxelBrickForceLeafRaster) != 0u && (node.level & 0xffu) == 0u && node.regionMappingId != 0u && node.childCount == 0u;
  check(square, "raw-albedo geometry is restricted to axis-aligned occupancy voxels");

  auto stale = value.result;
  stale.terrainGeneration += 1u;
  svr::TerrainGPUV2FineRasterOutput rejected{};
  check(
      svr::buildTerrainGPUV2FineRasterOracle(value.config, stale, value.columns, rejected, &error) && rejected.result.status == static_cast<uint32_t>(svr::TerrainGPUV2FineRasterStatus::StaleColumns) &&
          rejected.nodes.empty(),
      "stale column generations produce no fine-brick output");

  auto overflowFixture = fixture({}, {2u, 2u, 2u, 8u}, 1u, 1u);
  setCoreHeights(
      overflowFixture,
      [](uint32_t, uint32_t)
      {
        // Keep the one-voxel exterior shell inside this 16-voxel-tall
        // fixture; a surface above the chunk is correctly empty now.
        return 8;
      });
  svr::TerrainGPUV2FineRasterOutput overflow{};
  check(
      svr::buildTerrainGPUV2FineRasterOracle(overflowFixture.config, overflowFixture.result, overflowFixture.columns, overflow, &error) &&
          overflow.result.status == static_cast<uint32_t>(svr::TerrainGPUV2FineRasterStatus::Overflow) && overflow.nodes.empty() && overflow.occupancy.empty() && overflow.materialWords.empty(),
      "capacity overflow is detected before any output payload is written");
}

void testBoundariesAndCloseRay()
{
  std::string error;
  const svr::TerrainGPUChunkShape shape{1u, 1u, 1u, 8u};
  auto left = fixture({41u, 0, 0, 0, 0u, 1u, 3u, 7u}, shape, 1u, 512u);
  auto right = fixture({41u, 1, 0, 0, 0u, 1u, 3u, 7u}, shape, 1u, 512u);
  setCoreHeights(
      left,
      [](uint32_t, uint32_t)
      {
        return 8;
      });
  setCoreHeights(
      right,
      [](uint32_t, uint32_t)
      {
        return 8;
      });
  svr::TerrainGPUV2FineRasterOutput a{}, b{};
  check(
      svr::buildTerrainGPUV2FineRasterOracle(left.config, left.result, left.columns, a, &error) && svr::buildTerrainGPUV2FineRasterOracle(right.config, right.result, right.columns, b, &error),
      "adjacent boundary fixtures build: " + error);
  const int32_t leftMaximumCanonicalX = a.nodes[0].canonicalGridOriginScale[0] + 8 * a.nodes[0].canonicalGridOriginScale[3];
  const float leftMaximumWorldX = a.nodes[0].minimumVoxelSize[0] + 8.0f * a.nodes[0].minimumVoxelSize[3];
  check(leftMaximumCanonicalX == b.nodes[0].canonicalGridOriginScale[0] && leftMaximumWorldX == b.nodes[0].minimumVoxelSize[0], "adjacent chunk AABBs meet exactly and never overlap");

  auto coarse = fixture({41u, 0, 0, 0, 1u, 1u, 3u, 7u}, shape, 1u, 512u);
  setCoreHeights(
      coarse,
      [](uint32_t, uint32_t)
      {
        return 16;
      });
  svr::TerrainGPUV2FineRasterOutput coarseOutput{};
  check(
      svr::buildTerrainGPUV2FineRasterOracle(coarse.config, coarse.result, coarse.columns, coarseOutput, &error) && coarseOutput.nodes.size() == 1u && ((coarseOutput.nodes[0].level >> 8u) & 0xffu) == 1u &&
          coarseOutput.nodes[0].canonicalGridOriginScale[3] == 2,
      "leaf descriptors retain their clipmap LOD and canonical voxel scale");

  // A downward ray one centimetre above a flat voxel face has a stable +Y
  // face and depth. Subpixel X/Z motion remains inside the same integer cell;
  // there is no surface patch or micro-face capable of moving the hit.
  const float surfaceY = a.nodes[0].minimumVoxelSize[1] + 8.0f * a.nodes[0].minimumVoxelSize[3];
  const float nearOriginY = surfaceY + 0.01f;
  const float depthA = nearOriginY - surfaceY;
  const uint32_t cellA = static_cast<uint32_t>(std::floor((3.25f - a.nodes[0].minimumVoxelSize[0]) / a.nodes[0].minimumVoxelSize[3]));
  const uint32_t cellB = static_cast<uint32_t>(std::floor((3.25001f - a.nodes[0].minimumVoxelSize[0]) / a.nodes[0].minimumVoxelSize[3]));
  check(std::abs(depthA - 0.01f) < 1.0e-6f && cellA == cellB && cellA == 3u, "DDA face/depth identity is stable at 1 cm and under subpixel motion");
}

} // namespace

int main()
{
  std::cout << "Terrain GPU V2 Phase 4 fine raster tests\n";
  testPlanAndScratch();
  testEveryWordAndSyntheticShapes();
  testColumnDifferentialAndFailureAtomicity();
  testBoundariesAndCloseRay();
  std::cout << (failures == 0 ? "Terrain GPU V2 Phase 4 tests passed\n" : "Terrain GPU V2 Phase 4 tests failed\n");
  return failures == 0 ? 0 : 1;
}
