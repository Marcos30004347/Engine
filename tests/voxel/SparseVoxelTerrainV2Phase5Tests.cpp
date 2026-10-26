#include "voxel/svraster/SparseVoxelTree64Reference.hpp"
#include "voxel/svraster/TerrainGPUV2Tree64.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
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
  svr::TerrainGPUV2FineRasterOutput raster{};
  svr::TerrainGPUV2Tree64ConfigGPU treeConfig{};
};

Fixture fixture(const uint32_t seed = 7u)
{
  Fixture value{};
  const svr::TerrainGPUChunkKey key{31u, 0, 0, 0, 0u, 1u, seed, 11u};
  const svr::TerrainGPUChunkShape shape{2u, 2u, 2u, 8u};
  svr::TerrainGPUV2ColumnConfigGPU columnsConfig{};
  columnsConfig.columnsX = 16u;
  columnsConfig.columnsZ = 16u;
  columnsConfig.halo = 2u;
  columnsConfig.heightUnitMetres = 1.0f;
  columnsConfig.canonicalStep = 1u;
  columnsConfig.expectedTerrainGeneration = 11u;
  svr::TerrainGPUV2FineRasterSettings rasterSettings{};
  rasterSettings.key = key;
  rasterSettings.shape = shape;
  rasterSettings.columns = columnsConfig;
  rasterSettings.capacity = {8u, 4096u};
  rasterSettings.finestVoxelSizeMetres = 1.0;
  rasterSettings.regionMappingId = 17u;
  std::string error;
  check(svr::planTerrainGPUV2FineRaster(rasterSettings, value.rasterConfig, &error), "Phase 5 raster fixture plans: " + error);
  const uint32_t stride = columnsConfig.columnsX + 2u * columnsConfig.halo;
  std::vector<svr::TerrainGPUV2ColumnGPU> columns(uint64_t{stride} * (columnsConfig.columnsZ + 2u * columnsConfig.halo));
  std::mt19937 random(seed);
  for (auto &column : columns)
    column = {0, 0, 0u, svr::TerrainGPUV2ColumnValid};
  for (uint32_t z = 0u; z < 16u; ++z)
    for (uint32_t x = 0u; x < 16u; ++x)
    {
      auto &column = columns[uint64_t{z + 2u} * stride + x + 2u];
      column.maximumHeightUnits = 1 + static_cast<int32_t>(random() % 16u);
      column.material = 1u + random() % 31u;
    }
  svr::TerrainGPUV2ColumnResultGPU columnResult{};
  columnResult.status = static_cast<uint32_t>(svr::TerrainGPUV2ColumnStatus::Ready);
  columnResult.terrainGeneration = 11u;
  columnResult.columnCount = static_cast<uint32_t>(columns.size());
  columnResult.writtenColumns = static_cast<uint32_t>(columns.size());
  check(svr::buildTerrainGPUV2FineRasterOracle(value.rasterConfig, columnResult, columns, value.raster, &error), "Phase 5 raster oracle builds: " + error);
  svr::TerrainGPUV2Tree64Settings treeSettings{};
  treeSettings.capacity = {65u, 65u};
  treeSettings.terrainSlot = 4u;
  treeSettings.nodePageSlot = 9u;
  treeSettings.nodePageGeneration = 3u;
  treeSettings.geometryRevision = 0x123456789abcdef0ull;
  check(svr::planTerrainGPUV2Tree64(value.rasterConfig, treeSettings, value.treeConfig, &error), "Phase 5 Tree64 fixture plans: " + error);
  return value;
}

void abiAndBudget()
{
  const auto value = fixture();
  check(sizeof(svr::TerrainGPUV2Tree64ConfigGPU) == 192u && sizeof(svr::TerrainGPUV2Tree64ResultGPU) == 128u, "Tree64 build and result ABIs are frozen");
  check(
      value.treeConfig.modelSide == 16u && value.treeConfig.levelCount == 2u && value.treeConfig.levelCounts[0] == 64u && value.treeConfig.levelCounts[1] == 1u && value.treeConfig.levelOffsets[1] == 0u &&
          value.treeConfig.levelOffsets[0] == 1u && value.treeConfig.candidateCount == 65u,
      "candidate ranges are root-to-leaf and Morton child groups are contiguous");
  svr::TerrainGPUV2Tree64Memory memory{};
  std::string error;
  check(
      svr::terrainGPUV2Tree64Memory(value.treeConfig, memory, &error) && memory.maskBytes == 65u * 8u && memory.scratchBytes != 0u && memory.outputBytes == 65u * 12u + 128u,
      "masks, scans, and inactive outputs have exact separate budgets: " + error);
  auto tooSmall = svr::TerrainGPUV2Tree64Settings{{64u, 65u}};
  svr::TerrainGPUV2Tree64ConfigGPU ignored{};
  check(!svr::planTerrainGPUV2Tree64(value.rasterConfig, tooSmall, ignored, &error), "candidate budget rejects the job before allocation");
}

svr::TerrainGPUV2Tree64Output build(const Fixture &value)
{
  svr::TerrainGPUV2Tree64Output output{};
  std::string error;
  check(svr::buildTerrainGPUV2Tree64Oracle(value.treeConfig, value.raster.result, value.raster.nodes, value.raster.occupancy, output, &error), "parallel Tree64 CPU oracle builds: " + error);
  check(
      output.result.status == static_cast<uint32_t>(svr::TerrainGPUV2Tree64Status::Ready) && svr::validateTerrainGPUV2Tree64(value.treeConfig, output, &error), "compacted Tree64 and O(1) descriptor validate: " + error);
  return output;
}

void exactDifferential()
{
  const auto value = fixture(99u);
  const auto output = build(value);
  svr::SparseVoxelTree64Chunk tree{value.treeConfig.modelSide, output.nodes};
  std::vector<uint8_t> dense(uint64_t{value.treeConfig.modelSide} * value.treeConfig.modelSide * value.treeConfig.modelSide, 0u);
  uint32_t rasterOccupied = 0u;
  for (uint32_t nodeIndex = 0u; nodeIndex < value.raster.result.nodeCount; ++nodeIndex)
  {
    const auto &node = value.raster.nodes[nodeIndex];
    const uint32_t bx = static_cast<uint32_t>((node.canonicalGridOriginScale[0] - value.treeConfig.chunkMinimumCanonicalX) / static_cast<int32_t>(value.treeConfig.canonicalStep));
    const uint32_t by = static_cast<uint32_t>((node.canonicalGridOriginScale[1] - value.treeConfig.chunkMinimumCanonicalY) / static_cast<int32_t>(value.treeConfig.canonicalStep));
    const uint32_t bz = static_cast<uint32_t>((node.canonicalGridOriginScale[2] - value.treeConfig.chunkMinimumCanonicalZ) / static_cast<int32_t>(value.treeConfig.canonicalStep));
    for (uint32_t word = 0u; word < 16u; ++word)
      for (uint32_t bit = 0u; bit < 32u; ++bit)
        if ((value.raster.occupancy[node.occupancyIndex].cells[word].bits & (1u << bit)) != 0u)
        {
          const uint32_t cell = word * 32u + bit;
          const uint32_t x = bx + (cell & 7u);
          const uint32_t y = by + ((cell >> 3u) & 7u);
          const uint32_t z = bz + (cell >> 6u);
          dense[x + value.treeConfig.modelSide * (y + value.treeConfig.modelSide * z)] = 1u;
          ++rasterOccupied;
        }
  }
  bool exact = true;
  for (uint32_t z = 0u; z < value.treeConfig.modelSide; ++z)
    for (uint32_t y = 0u; y < value.treeConfig.modelSide; ++y)
      for (uint32_t x = 0u; x < value.treeConfig.modelSide; ++x)
        exact &= svr::sparseVoxelTree64Occupied(tree, {x, y, z}) == (dense[x + value.treeConfig.modelSide * (y + value.treeConfig.modelSide * z)] != 0u);
  check(exact, "Tree64 is an exact semantic differential of all 4,096 raster cells");
  check(output.result.occupiedVoxelCount == rasterOccupied && output.result.rasterOccupiedVoxelCount == rasterOccupied, "raster and Tree64 occupied counts agree exactly");

  const svr::SparseVoxelTree64Ray boundaryRay{{-1.0, 7.5, 7.5}, {1.0, 0.0, 0.0}, 32.0};
  const auto treeHit = svr::traceSparseVoxelTree64(tree, boundaryRay);
  const auto ddaHit = svr::traceSparseVoxelTree64CellsDDA(value.treeConfig.modelSide, dense, boundaryRay);
  check(treeHit.status == ddaHit.status && (!treeHit.hit() || treeHit.cell == ddaHit.cell), "Tree64 ray and dense DDA agree across a terminal/chunk-aligned boundary");
}

void rejectionAndDeterminism()
{
  const auto value = fixture(123u);
  const auto a = build(value);
  const auto b = build(value);
  check(a.nodes.size() == b.nodes.size() && std::memcmp(a.nodes.data(), b.nodes.data(), a.nodes.size() * sizeof(a.nodes[0])) == 0, "Morton compaction is byte deterministic");
  auto stale = value.raster.result;
  ++stale.terrainGeneration;
  svr::TerrainGPUV2Tree64Output rejected{};
  std::string error;
  check(
      svr::buildTerrainGPUV2Tree64Oracle(value.treeConfig, stale, value.raster.nodes, value.raster.occupancy, rejected, &error) &&
          rejected.result.status == static_cast<uint32_t>(svr::TerrainGPUV2Tree64Status::StaleRaster) && rejected.nodes.empty(),
      "stale raster generations cannot construct or publish Tree64 payload");
  auto overflowConfig = value.treeConfig;
  overflowConfig.nodeCapacity = 1u;
  svr::TerrainGPUV2Tree64Output overflow{};
  check(
      svr::buildTerrainGPUV2Tree64Oracle(overflowConfig, value.raster.result, value.raster.nodes, value.raster.occupancy, overflow, &error) &&
          overflow.result.status == static_cast<uint32_t>(svr::TerrainGPUV2Tree64Status::Overflow) && overflow.nodes.empty(),
      "node overflow is detected before the inactive output is written");
}

void randomizedLargeConservative()
{
  svr::TerrainGPUV2FineRasterConfigGPU rasterConfig{};
  rasterConfig.bricksX = 4u;
  rasterConfig.bricksY = 2u;
  rasterConfig.bricksZ = 4u;
  rasterConfig.brickSide = 8u;
  rasterConfig.denseBrickCount = 32u;
  rasterConfig.nodeCapacity = 32u;
  rasterConfig.canonicalStep = 2u;
  rasterConfig.voxelSizeMetres = 0.5f;
  rasterConfig.expectedTerrainGeneration = 27u;
  rasterConfig.regionMappingId = 3u;
  svr::TerrainGPUV2FineRasterResultGPU rasterResult{};
  rasterResult.status = static_cast<uint32_t>(svr::TerrainGPUV2FineRasterStatus::Ready);
  rasterResult.nodeCount = 32u;
  rasterResult.rootCount = 32u;
  rasterResult.terrainGeneration = 27u;
  std::vector<svr::SparseVoxelBrickNodeGPU> nodes(32u);
  std::vector<svr::SparseVoxelBrickOccupancyGPU> occupancy(32u);
  std::mt19937 random(0x51deu);
  uint32_t nodeIndex = 0u;
  std::vector<std::array<uint32_t, 3u>> occupiedCells;
  for (uint32_t x = 0u; x < 4u; ++x)
    for (uint32_t y = 0u; y < 2u; ++y)
      for (uint32_t z = 0u; z < 4u; ++z, ++nodeIndex)
      {
        nodes[nodeIndex].occupancyIndex = nodeIndex;
        nodes[nodeIndex].canonicalGridOriginScale = {static_cast<int32_t>(x * 16u), static_cast<int32_t>(y * 16u), static_cast<int32_t>(z * 16u), 2};
        for (uint32_t sample = 0u; sample < 23u; ++sample)
        {
          const uint32_t cell = random() % 512u;
          auto &bits = occupancy[nodeIndex].cells[cell / 32u].bits;
          if ((bits & (1u << (cell & 31u))) == 0u)
          {
            bits |= 1u << (cell & 31u);
            occupiedCells.push_back({x * 8u + (cell & 7u), y * 8u + ((cell >> 3u) & 7u), z * 8u + (cell >> 6u)});
          }
        }
      }
  rasterResult.occupiedVoxelCount = static_cast<uint32_t>(occupiedCells.size());
  rasterResult.materialCount = rasterResult.occupiedVoxelCount;
  svr::TerrainGPUV2Tree64Settings settings{};
  settings.capacity = {4161u, 4161u};
  svr::TerrainGPUV2Tree64ConfigGPU config{};
  std::string error;
  check(svr::planTerrainGPUV2Tree64(rasterConfig, settings, config, &error) && config.modelSide == 64u && config.candidateCount == 4161u, "randomized large fixture plans a multi-block, three-level Tree64: " + error);
  svr::TerrainGPUV2Tree64Output output{};
  check(
      svr::buildTerrainGPUV2Tree64Oracle(config, rasterResult, nodes, occupancy, output, &error) && output.result.status == static_cast<uint32_t>(svr::TerrainGPUV2Tree64Status::Ready),
      "randomized large Tree64 builds: " + error);
  const svr::SparseVoxelTree64Chunk tree{config.modelSide, output.nodes};
  bool conservative = true;
  for (const auto &cell : occupiedCells)
    conservative &= svr::sparseVoxelTree64Occupied(tree, cell);
  check(conservative && output.result.occupiedVoxelCount == occupiedCells.size(), "large randomized hierarchy conservatively retains every raster voxel without duplication");

  auto emptyResult = rasterResult;
  emptyResult.nodeCount = emptyResult.rootCount = emptyResult.occupiedVoxelCount = emptyResult.materialCount = 0u;
  svr::TerrainGPUV2Tree64Output empty{};
  check(
      svr::buildTerrainGPUV2Tree64Oracle(config, emptyResult, {}, {}, empty, &error) && empty.result.status == static_cast<uint32_t>(svr::TerrainGPUV2Tree64Status::Ready) && empty.nodes.size() == 1u &&
          svr::sparseVoxelTree64PopulationMask(empty.nodes[0]) == 0u,
      "empty chunks retain one valid empty root and no phantom occupancy");
}
} // namespace

int main()
{
  std::cout << "Sparse voxel terrain GPU V2 Phase 5 tests\n";
  abiAndBudget();
  exactDifferential();
  rejectionAndDeterminism();
  randomizedLargeConservative();
  if (failures != 0)
    std::cerr << failures << " Phase 5 checks failed\n";
  return failures == 0 ? 0 : 1;
}
