#include "os/Logger.hpp"
#include "procedural/world/MapExport.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "voxel/svraster/TerrainGPUV2FineRaster.hpp"
#include "voxel/svraster/PlanetVoxelChunk.hpp"
#include "voxel/svraster/rendering/TerrainGPUV2FineRasterChain.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace svr = voxel::svraster;
namespace svrRendering = voxel::svraster::rendering;
namespace world = procedural::world;

namespace
{
int failures = 0;

void check(const bool condition, const std::string &message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

template <typename T> std::vector<T> read(::rendering::RenderGraph &graph, const ::rendering::Buffer buffer, const uint32_t count)
{
  std::vector<T> output(count);
  graph.bufferRead(
      buffer,
      0u,
      uint64_t{count} * sizeof(T),
      [&](const void *data)
      {
        std::memcpy(output.data(), data, output.size() * sizeof(T));
      });
  return output;
}

template <typename T> bool prefixEqual(const std::vector<T> &left, const std::vector<T> &right, const uint32_t count)
{
  return left.size() >= count && right.size() >= count && (count == 0u || std::memcmp(left.data(), right.data(), uint64_t{count} * sizeof(T)) == 0);
}

struct Fixture
{
  svr::TerrainGPUV2FineRasterConfigGPU config{};
  svr::TerrainGPUV2ColumnResultGPU columnResult{};
  std::vector<svr::TerrainGPUV2ColumnGPU> columns{};
  svr::TerrainGPUV2FineRasterOutput expected{};
};

struct GPUOutput
{
  svr::TerrainGPUV2FineRasterResultGPU result{};
  std::vector<svr::SparseVoxelBrickNodeGPU> nodes{};
  std::vector<svr::SparseVoxelBrickOccupancyGPU> occupancy{};
  std::vector<uint32_t> materials{};
  std::vector<uint32_t> roots{};
};

Fixture fixture(const uint32_t internalLOD = 0u)
{
  Fixture value{};
  const uint32_t step = 1u << internalLOD;
  const svr::TerrainGPUChunkKey key{
    41u, 0, 0, 0, static_cast<uint16_t>(internalLOD), 1u, 3u, 7u};
  const svr::TerrainGPUChunkShape shape{2u, 2u, 2u, 8u};
  svr::TerrainGPUV2ColumnConfigGPU columnConfig{};
  columnConfig.columnsX = 16u;
  columnConfig.columnsZ = 16u;
  columnConfig.halo = 2u;
  columnConfig.heightUnitMetres = 1.0f;
  columnConfig.canonicalStep = step;
  columnConfig.tileMinimumCanonicalX = 0;
  columnConfig.tileMinimumCanonicalZ = 0;
  columnConfig.expectedTerrainGeneration = 7u;
  svr::TerrainGPUV2FineRasterSettings settings{};
  settings.key = key;
  settings.shape = shape;
  settings.columns = columnConfig;
  settings.capacity = {8u, 4096u};
  settings.finestVoxelSizeMetres = 1.0;
  settings.regionMappingId = 29u;
  settings.paletteBase = 11u;
  settings.surfaceShellDepthVoxels = 1u;
  std::string error;
  check(svr::planTerrainGPUV2FineRaster(settings, value.config, &error), "GPU fine raster fixture plans: " + error);
  const uint32_t outerX = columnConfig.columnsX + 2u * columnConfig.halo;
  const uint32_t outerZ = columnConfig.columnsZ + 2u * columnConfig.halo;
  value.columns.resize(outerX * outerZ);
  for (auto &column : value.columns)
    column = {0, 0, 0u, svr::TerrainGPUV2ColumnValid};
  for (uint32_t z = 0u; z < columnConfig.columnsZ; ++z)
    for (uint32_t x = 0u; x < columnConfig.columnsX; ++x)
    {
      auto &column = value.columns[uint64_t{z + columnConfig.halo} * outerX + x + columnConfig.halo];
      const int32_t lowerCell = 3 + static_cast<int32_t>((x + z) % 7u);
      column.minimumHeightUnits = lowerCell * static_cast<int32_t>(step) +
          static_cast<int32_t>((internalLOD != 0u && ((x + z) & 1u) != 0u)
              ? step / 2u : 0u);
      column.maximumHeightUnits = ((x + z) % 7u) == 0u
          ? column.minimumHeightUnits
          : column.minimumHeightUnits +
              static_cast<int32_t>((1u + (x * 3u + z * 5u) % 4u) * step);
      column.material = 1u + (x + z) % 23u;
    }
  value.columnResult.status = static_cast<uint32_t>(svr::TerrainGPUV2ColumnStatus::Ready);
  value.columnResult.terrainGeneration = 7u;
  value.columnResult.columnCount = outerX * outerZ;
  value.columnResult.writtenColumns = outerX * outerZ;
  check(svr::buildTerrainGPUV2FineRasterOracle(value.config, value.columnResult, value.columns, value.expected, &error), "GPU differential oracle builds: " + error);
  return value;
}

int32_t floorDivision(const int32_t value, const int32_t divisor)
{
  const int32_t quotient = value / divisor;
  const int32_t remainder = value % divisor;
  return quotient - (remainder < 0 ? 1 : 0);
}

svr::TerrainGPUV2ColumnBuildSettings worldColumnSettings(
    const world::MapExportBundle &source,
    const double u,
    const double v,
    const uint32_t lod,
    const uint32_t columns = 64u)
{
  constexpr double finestVoxelMetres = 0.1;
  const uint32_t canonicalStep = uint32_t{1u} << lod;
  const uint32_t worldExtentCanonical = static_cast<uint32_t>(
      std::llround(source.extentMetres / finestVoxelMetres));
  const int64_t tileSpan = int64_t{columns} * canonicalStep;
  svr::TerrainGPUV2ColumnBuildSettings settings{};
  settings.key.worldBundleRevision = source.bundleSignature;
  settings.key.tileX = static_cast<int32_t>(
      std::floor(std::clamp(u, 0.0, 1.0) * worldExtentCanonical) /
      tileSpan);
  settings.key.tileZ = static_cast<int32_t>(
      std::floor(std::clamp(v, 0.0, 1.0) * worldExtentCanonical) /
      tileSpan);
  settings.key.clipmapLOD = lod;
  settings.key.terrainSlot = 5u;
  settings.key.expectedGeneration = 23u;
  settings.key.shape = {static_cast<uint16_t>(columns),
                        static_cast<uint16_t>(columns), 2u, 0u};
  settings.requestedMapLevel = source.maximumPageLevel;
  settings.worldExtentCanonical = worldExtentCanonical;
  settings.heightUnitMetres = static_cast<float>(finestVoxelMetres);
  settings.proceduralDetailStrength = 2.0;
  settings.proceduralTerraceStrength = 1.0;
  settings.proceduralTerraceHeightMetres = 4.0;
  settings.proceduralTerraceTransitionFraction = 0.16;
  settings.expectedAtlasRevision = 11u;
  settings.faultCapacity = 4096u;
  return settings;
}

uint32_t requiredDynamicVerticalCells(
    const std::vector<svr::TerrainGPUV2ColumnGPU> &columns,
    const uint32_t canonicalStep)
{
  int32_t minimum = std::numeric_limits<int32_t>::max();
  int32_t maximum = std::numeric_limits<int32_t>::min();
  for (const auto &column : columns)
  {
    if ((column.flags & svr::TerrainGPUV2ColumnValid) == 0u)
      continue;
    minimum = std::min(minimum, column.minimumHeightUnits);
    maximum = std::max(maximum, column.maximumHeightUnits);
  }
  if (maximum < minimum || canonicalStep == 0u)
    return 0u;
  // GPU-v2 anchors a page one cell below the minimum aligned source height.
  // The exclusive upper surface boundary is rounded up.  Both cells belong
  // to the Tree64 model and must fit; silently truncating the upper one opens
  // exactly the light window this real-map fixture is intended to catch.
  const int64_t alignedMinimum =
      int64_t{floorDivision(minimum, static_cast<int32_t>(canonicalStep))} *
          canonicalStep -
      canonicalStep;
  const int64_t alignedMaximumExclusive =
      -int64_t{floorDivision(-maximum, static_cast<int32_t>(canonicalStep))} *
      canonicalStep;
  return static_cast<uint32_t>(std::max<int64_t>(
      0, (alignedMaximumExclusive - alignedMinimum) / canonicalStep));
}

std::vector<Fixture> worldRasterFixtures(
    const world::MapExportBundle &source,
    const world::MapExportGpuPageLayout &layout,
    const double u,
    const double v,
    const uint32_t lod)
{
  std::vector<Fixture> fixtures;
  std::string error;
  const auto columnSettings = worldColumnSettings(source, u, v, lod);
  svr::TerrainGPUV2ColumnConfigGPU columnsConfig{};
  std::vector<svr::TerrainGPUV2HeightSampleGPU> heights;
  std::vector<svr::TerrainGPUV2ColumnGPU> columns;
  check(svr::planTerrainGPUV2Columns(
            source, layout, columnSettings, columnsConfig, &error) &&
            svr::buildTerrainGPUV2ColumnOracle(
                source, columnsConfig, heights, columns, &error),
        "real-map raster input columns build at LOD " +
            std::to_string(lod) + ": " + error);
  if (columns.empty())
    return fixtures;

  int32_t minimumHeight = std::numeric_limits<int32_t>::max();
  int32_t maximumHeight = std::numeric_limits<int32_t>::min();
  for (const auto &column : columns)
  {
    if ((column.flags & svr::TerrainGPUV2ColumnValid) == 0u)
      continue;
    minimumHeight = std::min(minimumHeight, column.minimumHeightUnits);
    maximumHeight = std::max(maximumHeight, column.maximumHeightUnits);
  }
  if (maximumHeight < minimumHeight)
  {
    check(false, "real-map raster input contains valid surface columns");
    return fixtures;
  }

  constexpr svr::TerrainGPUChunkShape shape{8u, 4u, 8u, 8u};
  const int32_t verticalSpan =
      static_cast<int32_t>(shape.bricksY * shape.brickSide) *
      static_cast<int32_t>(columnsConfig.canonicalStep);
  const int32_t firstChunkY = floorDivision(minimumHeight, verticalSpan);
  const int32_t lastChunkY = floorDivision(maximumHeight, verticalSpan);
  for (int32_t chunkY = firstChunkY; chunkY <= lastChunkY; ++chunkY)
  {
    Fixture fixture{};
    fixture.columns = columns;
    fixture.columnResult.status =
        static_cast<uint32_t>(svr::TerrainGPUV2ColumnStatus::Ready);
    fixture.columnResult.terrainGeneration =
        columnSettings.key.expectedGeneration;
    fixture.columnResult.columnCount = static_cast<uint32_t>(columns.size());
    fixture.columnResult.writtenColumns =
        static_cast<uint32_t>(columns.size());
    svr::TerrainGPUV2FineRasterSettings raster{};
    raster.key = {
      source.bundleSignature,
      columnSettings.key.tileX,
      chunkY,
      columnSettings.key.tileZ,
      static_cast<uint16_t>(lod),
      static_cast<uint16_t>(columnSettings.key.terrainSlot),
      19u,
      columnSettings.key.expectedGeneration};
    raster.shape = shape;
    raster.columns = columnsConfig;
    raster.capacity = {
      uint32_t{shape.bricksX} * shape.bricksY * shape.bricksZ,
      uint32_t{shape.bricksX} * shape.bricksY * shape.bricksZ * 512u};
    raster.worldOriginMetres = {
      -source.extentMetres * 0.5, 0.0, -source.extentMetres * 0.5};
    raster.finestVoxelSizeMetres = 0.1;
    raster.regionMappingId = 19u;
    raster.surfaceShellDepthVoxels = 1u;
    raster.boundaryStitchSubdivisions = lod > 0u ? 2u : 1u;
    check(svr::planTerrainGPUV2FineRaster(
              raster, fixture.config, &error),
          "real-map vertical raster chunk plans at LOD " +
              std::to_string(lod) + ": " + error);
    check(svr::buildTerrainGPUV2FineRasterOracle(
              fixture.config, fixture.columnResult, fixture.columns,
              fixture.expected, &error),
          "real-map CPU fine-raster oracle builds at LOD " +
              std::to_string(lod) + ": " + error);
    fixtures.push_back(std::move(fixture));
  }
  return fixtures;
}

bool matchesCPUColumnGeneration(const Fixture &source, const GPUOutput &actual)
{
  if (actual.result.status !=
          static_cast<uint32_t>(svr::TerrainGPUV2FineRasterStatus::Ready))
    return false;
  const uint32_t voxelsX = source.config.bricksX * source.config.brickSide;
  const uint32_t voxelsY = source.config.bricksY * source.config.brickSide;
  const uint32_t voxelsZ = source.config.bricksZ * source.config.brickSide;
  std::vector<uint8_t> occupied(uint64_t{voxelsX} * voxelsY * voxelsZ, 0u);
  const auto address = [&](const uint32_t x, const uint32_t y,
                           const uint32_t z)
  {
    return uint64_t{x} + uint64_t{voxelsX} *
        (uint64_t{y} + uint64_t{voxelsY} * z);
  };
  for (uint32_t nodeIndex = 0u; nodeIndex < actual.result.nodeCount;
       ++nodeIndex)
  {
    const auto &node = actual.nodes[nodeIndex];
    if (node.occupancyIndex >= actual.occupancy.size())
      return false;
    std::array<int32_t, 3u> base{};
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      const int32_t minimum = axis == 0u
          ? source.config.chunkMinimumCanonicalX
          : axis == 1u ? source.config.chunkMinimumCanonicalY
                       : source.config.chunkMinimumCanonicalZ;
      const int32_t delta = node.canonicalGridOriginScale[axis] - minimum;
      if (delta < 0 || delta % static_cast<int32_t>(source.config.canonicalStep) != 0)
        return false;
      base[axis] = delta / static_cast<int32_t>(source.config.canonicalStep);
    }
    const auto &mask = actual.occupancy[node.occupancyIndex];
    for (uint32_t word = 0u; word < 16u; ++word)
    {
      uint32_t bits = mask.cells[word].bits;
      uint32_t prior = 0u;
      for (uint32_t previous = 0u; previous < word; ++previous)
        prior += static_cast<uint32_t>(__builtin_popcount(mask.cells[previous].bits));
      while (bits != 0u)
      {
        const uint32_t bit = static_cast<uint32_t>(__builtin_ctz(bits));
        const uint32_t preceding = bit == 0u ? 0u :
            mask.cells[word].bits & ((1u << bit) - 1u);
        bits &= bits - 1u;
        const uint32_t cell = word * 32u + bit;
        const uint32_t x = static_cast<uint32_t>(base[0]) + (cell & 7u);
        const uint32_t y = static_cast<uint32_t>(base[1]) + ((cell >> 3u) & 7u);
        const uint32_t z = static_cast<uint32_t>(base[2]) + (cell >> 6u);
        if (x >= voxelsX || y >= voxelsY || z >= voxelsZ ||
            occupied[address(x, y, z)] != 0u)
          return false;
        occupied[address(x, y, z)] = 1u;
        const auto &column = source.columns[
            uint64_t{z + source.config.halo} * source.config.columnStride +
            x + source.config.halo];
        const uint32_t materialAddress = node.materialBase + prior +
            static_cast<uint32_t>(__builtin_popcount(preceding));
        if (svr::terrainGPUV2FineRasterMaterial(
                actual.materials, materialAddress) != column.material)
          return false;
      }
    }
  }
  const int64_t step = source.config.canonicalStep;
  const auto topCellAt = [&](const int32_t x, const int32_t z)
  {
    const int32_t clampedX = std::clamp(
        x, -static_cast<int32_t>(source.config.halo),
        static_cast<int32_t>(voxelsX + source.config.halo) - 1);
    const int32_t clampedZ = std::clamp(
        z, -static_cast<int32_t>(source.config.halo),
        static_cast<int32_t>(voxelsZ + source.config.halo) - 1);
    const auto &sample = source.columns[
        static_cast<uint64_t>(
            clampedZ + static_cast<int32_t>(source.config.halo)) *
            source.config.columnStride +
        clampedX + static_cast<int32_t>(source.config.halo)];
    const int64_t maximum = int64_t{sample.maximumHeightUnits} -
        source.config.chunkMinimumCanonicalY;
    return maximum > 0 ? (maximum + step - 1) / step - 1
                       : -((-maximum) / step) - 1;
  };
  for (uint32_t z = 0u; z < voxelsZ; ++z)
    for (uint32_t x = 0u; x < voxelsX; ++x)
    {
      const auto &column = source.columns[
          uint64_t{z + source.config.halo} * source.config.columnStride +
          x + source.config.halo];
      const int64_t origin = source.config.chunkMinimumCanonicalY;
      const int64_t minimum = int64_t{column.minimumHeightUnits} - origin;
      const int64_t maximum = int64_t{column.maximumHeightUnits} - origin;
      const int64_t floorCell = minimum >= 0 ? minimum / step
          : -((-minimum + step - 1) / step);
      const int64_t topCell = maximum > 0 ? (maximum + step - 1) / step - 1
          : -((-maximum) / step) - 1;
      const int64_t ownedFloorCell = std::min(floorCell, topCell);
      const int64_t lowestNeighbour = std::min({
          topCell,
          topCellAt(static_cast<int32_t>(x) - 1, z),
          topCellAt(static_cast<int32_t>(x) + 1, z),
          topCellAt(x, static_cast<int32_t>(z) - 1),
          topCellAt(x, static_cast<int32_t>(z) + 1)});
      const int64_t firstExposedSide = lowestNeighbour < topCell
          ? lowestNeighbour + 1 : topCell;
      const int64_t closureFloor = std::max<int64_t>(
          0, firstExposedSide - source.config.surfaceShellDepthVoxels);
      const int64_t firstCell = std::min(ownedFloorCell, closureFloor);
      for (uint32_t y = 0u; y < voxelsY; ++y)
      {
        const bool expected = (column.flags & svr::TerrainGPUV2ColumnValid) != 0u &&
            int64_t{y} >= firstCell && int64_t{y} <= topCell;
        if ((occupied[address(x, y, z)] != 0u) != expected)
          return false;
      }
    }
  return true;
}

GPUOutput runVariant(::rendering::backend::vulkan::VulkanRHI &rhi, const Fixture &source, const uint32_t workgroup, const bool testFailures)
{
  using namespace ::rendering;
  GPUOutput output{};
  RenderGraph graph(&rhi);
  const auto input = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push;
  const Buffer columns = graph.createBuffer({.name = "TerrainGPUV2.P4.Columns", .size = source.columns.size() * sizeof(svr::TerrainGPUV2ColumnGPU), .usage = input});
  const Buffer columnResult = graph.createBuffer({.name = "TerrainGPUV2.P4.ColumnResult", .size = sizeof(svr::TerrainGPUV2ColumnResultGPU), .usage = input});
  graph.bufferWrite(columns, 0u, source.columns.size() * sizeof(svr::TerrainGPUV2ColumnGPU), const_cast<svr::TerrainGPUV2ColumnGPU *>(source.columns.data()));
  graph.bufferWrite(columnResult, 0u, sizeof(source.columnResult), const_cast<svr::TerrainGPUV2ColumnResultGPU *>(&source.columnResult));
  const auto pull = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst;
  const Buffer resultReadback = graph.createBuffer({.name = "TerrainGPUV2.P4.ResultReadback", .size = sizeof(svr::TerrainGPUV2FineRasterResultGPU), .usage = pull});
  const Buffer nodeReadback = graph.createBuffer({.name = "TerrainGPUV2.P4.NodeReadback", .size = uint64_t{source.config.nodeCapacity} * sizeof(svr::SparseVoxelBrickNodeGPU), .usage = pull});
  const Buffer occupancyReadback = graph.createBuffer({.name = "TerrainGPUV2.P4.OccupancyReadback", .size = uint64_t{source.config.nodeCapacity} * sizeof(svr::SparseVoxelBrickOccupancyGPU), .usage = pull});
  const Buffer materialReadback = graph.createBuffer({.name = "TerrainGPUV2.P4.MaterialReadback", .size = uint64_t{source.config.materialWordCapacity} * sizeof(uint32_t), .usage = pull});
  const Buffer rootReadback = graph.createBuffer({.name = "TerrainGPUV2.P4.RootReadback", .size = uint64_t{source.config.nodeCapacity} * sizeof(uint32_t), .usage = pull});
  uint32_t passIndex = 0u;
  {
    svrRendering::TerrainGPUV2FineRasterChain chain(graph, passIndex, {columns, columnResult}, source.config, workgroup);
    auto *copyResult = graph.registerPass<gpgpu::CopyBufferPass>(
        "TerrainGPUV2.P4.CopyResult", passIndex++, chain.result(), 0u, sizeof(svr::TerrainGPUV2FineRasterResultGPU), resultReadback, 0u, sizeof(svr::TerrainGPUV2FineRasterResultGPU));
    auto *copyNodes = graph.registerPass<gpgpu::CopyBufferPass>(
        "TerrainGPUV2.P4.CopyNodes",
        passIndex++,
        chain.nodes(),
        0u,
        uint64_t{source.config.nodeCapacity} * sizeof(svr::SparseVoxelBrickNodeGPU),
        nodeReadback,
        0u,
        uint64_t{source.config.nodeCapacity} * sizeof(svr::SparseVoxelBrickNodeGPU));
    auto *copyOccupancy = graph.registerPass<gpgpu::CopyBufferPass>(
        "TerrainGPUV2.P4.CopyOccupancy",
        passIndex++,
        chain.occupancy(),
        0u,
        uint64_t{source.config.nodeCapacity} * sizeof(svr::SparseVoxelBrickOccupancyGPU),
        occupancyReadback,
        0u,
        uint64_t{source.config.nodeCapacity} * sizeof(svr::SparseVoxelBrickOccupancyGPU));
    auto *copyMaterials = graph.registerPass<gpgpu::CopyBufferPass>(
        "TerrainGPUV2.P4.CopyMaterials",
        passIndex++,
        chain.materialWords(),
        0u,
        uint64_t{source.config.materialWordCapacity} * sizeof(uint32_t),
        materialReadback,
        0u,
        uint64_t{source.config.materialWordCapacity} * sizeof(uint32_t));
    auto *copyRoots = graph.registerPass<gpgpu::CopyBufferPass>(
        "TerrainGPUV2.P4.CopyRoots", passIndex++, chain.rootIndices(), 0u, uint64_t{source.config.nodeCapacity} * sizeof(uint32_t), rootReadback, 0u, uint64_t{source.config.nodeCapacity} * sizeof(uint32_t));
    check(chain.update(source.config), "GPU fine raster chain accepts the planned config");
    check(chain.microGeometryBytes() == 0u && chain.scratchBytes() != 0u && chain.outputBytes() != 0u, "GPU chain budgets scratch/output and allocates no microgeometry");
    graph.compile();
    const auto run = [&]()
    {
      RenderGraph::Frame frame{};
      RenderGraph::Overrides overrides{};
      graph.run(frame, overrides);
      graph.waitFrame(frame);
    };
    const auto capture = [&]()
    {
      GPUOutput result{};
      result.result = read<svr::TerrainGPUV2FineRasterResultGPU>(graph, resultReadback, 1u).front();
      result.nodes = read<svr::SparseVoxelBrickNodeGPU>(graph, nodeReadback, source.config.nodeCapacity);
      result.occupancy = read<svr::SparseVoxelBrickOccupancyGPU>(graph, occupancyReadback, source.config.nodeCapacity);
      result.materials = read<uint32_t>(graph, materialReadback, source.config.materialWordCapacity);
      result.roots = read<uint32_t>(graph, rootReadback, source.config.nodeCapacity);
      return result;
    };
    run();
    output = capture();
    if (testFailures)
    {
      const auto prior = output;
      auto stale = source.columnResult;
      ++stale.terrainGeneration;
      graph.bufferWrite(columnResult, 0u, sizeof(stale), &stale);
      run();
      const auto rejected = capture();
      check(
          rejected.result.status == static_cast<uint32_t>(svr::TerrainGPUV2FineRasterStatus::StaleColumns) && prefixEqual(rejected.nodes, prior.nodes, prior.result.nodeCount) &&
              prefixEqual(rejected.occupancy, prior.occupancy, prior.result.nodeCount) && prefixEqual(rejected.materials, prior.materials, (prior.result.materialCount + 3u) / 4u),
          "stale GPU columns leave the previous fine-brick payload untouched");
      auto verticallyOverflowing = source.columnResult;
      verticallyOverflowing.reserved2 = 1u;
      const int32_t minimumHeight = 0;
      const int32_t maximumHeight = static_cast<int32_t>(
          (source.config.bricksY * source.config.brickSide + 2u) *
          source.config.canonicalStep);
      std::memcpy(&verticallyOverflowing.reserved0, &minimumHeight,
          sizeof(minimumHeight));
      std::memcpy(&verticallyOverflowing.reserved1, &maximumHeight,
          sizeof(maximumHeight));
      graph.bufferWrite(columnResult, 0u, sizeof(verticallyOverflowing),
          &verticallyOverflowing);
      run();
      const auto verticalOverflow = capture();
      check(
          verticalOverflow.result.status == static_cast<uint32_t>(
              svr::TerrainGPUV2FineRasterStatus::Overflow) &&
              verticalOverflow.result.overflowStage == static_cast<uint32_t>(
                  svr::TerrainGPUV2FineRasterOverflowStage::Vertical) &&
              prefixEqual(verticalOverflow.nodes, prior.nodes,
                  prior.result.nodeCount) &&
              prefixEqual(verticalOverflow.occupancy, prior.occupancy,
                  prior.result.nodeCount),
          "dynamic vertical overflow is rejected instead of publishing a "
          "Tree64 page with missing upper voxels");
      graph.bufferWrite(columnResult, 0u, sizeof(source.columnResult), const_cast<svr::TerrainGPUV2ColumnResultGPU *>(&source.columnResult));
      auto invalidColumns = source.columns;
      invalidColumns[uint64_t{source.config.halo} * source.config.columnStride + source.config.halo].material = 256u;
      graph.bufferWrite(columns, 0u, invalidColumns.size() * sizeof(svr::TerrainGPUV2ColumnGPU), invalidColumns.data());
      run();
      const auto invalid = capture();
      check(
          invalid.result.status == static_cast<uint32_t>(svr::TerrainGPUV2FineRasterStatus::ValidationFailed) && invalid.result.validationErrors != 0u && prefixEqual(invalid.nodes, prior.nodes, prior.result.nodeCount) &&
              prefixEqual(invalid.occupancy, prior.occupancy, prior.result.nodeCount),
          "out-of-range GPU materials fail validation without modifying compact output");
      graph.bufferWrite(columns, 0u, source.columns.size() * sizeof(svr::TerrainGPUV2ColumnGPU), const_cast<svr::TerrainGPUV2ColumnGPU *>(source.columns.data()));
      auto overflow = source.config;
      overflow.nodeCapacity = 1u;
      overflow.materialCapacity = 1u;
      overflow.materialWordCapacity = 1u;
      check(chain.update(overflow), "GPU fine raster accepts a smaller per-job output budget");
      run();
      const auto overflowed = capture();
      check(
          overflowed.result.status == static_cast<uint32_t>(svr::TerrainGPUV2FineRasterStatus::Overflow) && overflowed.result.overflowStage == static_cast<uint32_t>(svr::TerrainGPUV2FineRasterOverflowStage::Nodes) &&
              prefixEqual(overflowed.nodes, prior.nodes, prior.result.nodeCount) && prefixEqual(overflowed.occupancy, prior.occupancy, prior.result.nodeCount),
          "GPU capacity overflow is rejected before compact outputs are modified");
    }
    delete copyRoots;
    delete copyMaterials;
    delete copyOccupancy;
    delete copyNodes;
    delete copyResult;
  }
  graph.deleteBuffer(rootReadback);
  graph.deleteBuffer(materialReadback);
  graph.deleteBuffer(occupancyReadback);
  graph.deleteBuffer(nodeReadback);
  graph.deleteBuffer(resultReadback);
  graph.deleteBuffer(columnResult);
  graph.deleteBuffer(columns);
  return output;
}

std::vector<GPUOutput> runBatch(
    ::rendering::backend::vulkan::VulkanRHI &rhi,
    const std::vector<Fixture> &sources)
{
  using namespace ::rendering;
  const uint32_t lanes = static_cast<uint32_t>(sources.size());
  const auto &capacity = sources.front().config;
  const uint32_t columnStride = static_cast<uint32_t>(sources.front().columns.size());
  std::vector<svr::TerrainGPUV2ColumnGPU> columns(uint64_t{lanes} * columnStride);
  std::vector<svr::TerrainGPUV2ColumnResultGPU> columnResults(lanes);
  std::vector<svr::TerrainGPUV2FineRasterConfigGPU> configs;
  for (uint32_t lane = 0u; lane < lanes; ++lane)
  {
    std::copy(sources[lane].columns.begin(), sources[lane].columns.end(), columns.begin() + uint64_t{lane} * columnStride);
    columnResults[lane] = sources[lane].columnResult;
    columnResults[lane].reserved3 = columnStride;
    configs.push_back(sources[lane].config);
  }
  RenderGraph graph(&rhi);
  const auto input = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push;
  const auto pull = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst;
  const Buffer columnsBuffer = graph.createBuffer({.name = "TerrainGPUV2.P4.BatchColumns", .size = columns.size() * sizeof(columns[0]), .usage = input});
  const Buffer columnResultBuffer = graph.createBuffer({.name = "TerrainGPUV2.P4.BatchColumnResults", .size = columnResults.size() * sizeof(columnResults[0]), .usage = input});
  graph.bufferWrite(columnsBuffer, 0u, columns.size() * sizeof(columns[0]), columns.data());
  graph.bufferWrite(columnResultBuffer, 0u, columnResults.size() * sizeof(columnResults[0]), columnResults.data());
  const uint64_t resultBytes = uint64_t{lanes} * sizeof(svr::TerrainGPUV2FineRasterResultGPU);
  const uint64_t nodeBytes = uint64_t{lanes} * capacity.nodeCapacity * sizeof(svr::SparseVoxelBrickNodeGPU);
  const uint64_t occupancyBytes = uint64_t{lanes} * capacity.nodeCapacity * sizeof(svr::SparseVoxelBrickOccupancyGPU);
  const uint64_t materialBytes = uint64_t{lanes} * capacity.materialWordCapacity * sizeof(uint32_t);
  const uint64_t rootBytes = uint64_t{lanes} * capacity.nodeCapacity * sizeof(uint32_t);
  const Buffer resultReadback = graph.createBuffer({.name = "TerrainGPUV2.P4.BatchResults", .size = resultBytes, .usage = pull});
  const Buffer nodeReadback = graph.createBuffer({.name = "TerrainGPUV2.P4.BatchNodes", .size = nodeBytes, .usage = pull});
  const Buffer occupancyReadback = graph.createBuffer({.name = "TerrainGPUV2.P4.BatchOccupancy", .size = occupancyBytes, .usage = pull});
  const Buffer materialReadback = graph.createBuffer({.name = "TerrainGPUV2.P4.BatchMaterials", .size = materialBytes, .usage = pull});
  const Buffer rootReadback = graph.createBuffer({.name = "TerrainGPUV2.P4.BatchRoots", .size = rootBytes, .usage = pull});
  uint32_t passIndex = 0u;
  std::vector<GPUOutput> outputs(lanes);
  {
    svrRendering::TerrainGPUV2FineRasterChain chain(graph, passIndex, {columnsBuffer, columnResultBuffer}, capacity, 128u, lanes, "TerrainGPUV2.P4.BatchFine");
    std::vector<Pass *> copies;
    auto copy = [&](const std::string &name, const Buffer source, const uint64_t bytes, const Buffer destination)
    {
      copies.push_back(graph.registerPass<gpgpu::CopyBufferPass>(name, passIndex++, source, 0u, bytes, destination, 0u, bytes));
    };
    copy("TerrainGPUV2.P4.BatchCopyResults", chain.result(), resultBytes, resultReadback);
    copy("TerrainGPUV2.P4.BatchCopyNodes", chain.nodes(), nodeBytes, nodeReadback);
    copy("TerrainGPUV2.P4.BatchCopyOccupancy", chain.occupancy(), occupancyBytes, occupancyReadback);
    copy("TerrainGPUV2.P4.BatchCopyMaterials", chain.materialWords(), materialBytes, materialReadback);
    copy("TerrainGPUV2.P4.BatchCopyRoots", chain.rootIndices(), rootBytes, rootReadback);
    check(chain.updateBatch(configs), "mixed fine-raster batch accepts every active lane");
    check(std::all_of(chain.indirectDispatchPlan().begin(), chain.indirectDispatchPlan().end(), [lanes](const auto &dispatch) { return dispatch.z == lanes; }),
        "every fine-raster stage dispatches the active lane count indirectly");
    graph.compile();
    RenderGraph::Frame frame{};
    RenderGraph::Overrides overrides{};
    graph.run(frame, overrides);
    graph.waitFrame(frame);
    const auto results = read<svr::TerrainGPUV2FineRasterResultGPU>(graph, resultReadback, lanes);
    const auto nodes = read<svr::SparseVoxelBrickNodeGPU>(graph, nodeReadback, lanes * capacity.nodeCapacity);
    const auto occupancy = read<svr::SparseVoxelBrickOccupancyGPU>(graph, occupancyReadback, lanes * capacity.nodeCapacity);
    const auto materials = read<uint32_t>(graph, materialReadback, lanes * capacity.materialWordCapacity);
    const auto roots = read<uint32_t>(graph, rootReadback, lanes * capacity.nodeCapacity);
    for (uint32_t lane = 0u; lane < lanes; ++lane)
    {
      outputs[lane].result = results[lane];
      outputs[lane].nodes.assign(nodes.begin() + uint64_t{lane} * capacity.nodeCapacity, nodes.begin() + uint64_t{lane + 1u} * capacity.nodeCapacity);
      outputs[lane].occupancy.assign(occupancy.begin() + uint64_t{lane} * capacity.nodeCapacity, occupancy.begin() + uint64_t{lane + 1u} * capacity.nodeCapacity);
      outputs[lane].materials.assign(materials.begin() + uint64_t{lane} * capacity.materialWordCapacity, materials.begin() + uint64_t{lane + 1u} * capacity.materialWordCapacity);
      outputs[lane].roots.assign(roots.begin() + uint64_t{lane} * capacity.nodeCapacity, roots.begin() + uint64_t{lane + 1u} * capacity.nodeCapacity);
    }
    for (auto iterator = copies.rbegin(); iterator != copies.rend(); ++iterator)
      delete *iterator;
  }
  graph.deleteBuffer(rootReadback); graph.deleteBuffer(materialReadback); graph.deleteBuffer(occupancyReadback);
  graph.deleteBuffer(nodeReadback); graph.deleteBuffer(resultReadback); graph.deleteBuffer(columnResultBuffer); graph.deleteBuffer(columnsBuffer);
  return outputs;
}

void testGPUFineRaster(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  const auto source = fixture();
  const auto output64 = runVariant(rhi, source, 64u, true);
  const auto matchesOracle = [&](const GPUOutput &actual)
  {
    return actual.result.status == static_cast<uint32_t>(svr::TerrainGPUV2FineRasterStatus::Ready) && actual.result.nodeCount == source.expected.result.nodeCount &&
           actual.result.materialCount == source.expected.result.materialCount && actual.result.rootCount == source.expected.result.rootCount && actual.result.flags == source.expected.result.flags &&
           actual.result.perNodeVoxelScratchWords == 0u && actual.result.microSurfaceBytes == 0u && prefixEqual(actual.nodes, source.expected.nodes, actual.result.nodeCount) &&
           prefixEqual(actual.occupancy, source.expected.occupancy, actual.result.nodeCount) && prefixEqual(actual.materials, source.expected.materialWords, (actual.result.materialCount + 3u) / 4u) &&
           prefixEqual(actual.roots, source.expected.rootIndices, actual.result.rootCount);
  };
  check(matchesOracle(output64) && output64.result.selectedWorkgroup == 64u, "64-thread GPU fine bricks byte-match the CPU oracle");
  const auto output128 = runVariant(rhi, source, 128u, false);
  const auto output256 = runVariant(rhi, source, 256u, false);
  check(matchesOracle(output128) && matchesOracle(output256), "128/256-thread GPU fine bricks byte-match the CPU oracle");
  check(
      prefixEqual(output64.nodes, output128.nodes, output64.result.nodeCount) && prefixEqual(output64.nodes, output256.nodes, output64.result.nodeCount) &&
          prefixEqual(output64.occupancy, output128.occupancy, output64.result.nodeCount) && prefixEqual(output64.occupancy, output256.occupancy, output64.result.nodeCount) &&
          prefixEqual(output64.materials, output128.materials, (output64.result.materialCount + 3u) / 4u) && prefixEqual(output64.materials, output256.materials, (output64.result.materialCount + 3u) / 4u),
      "dispatch order and all compiled workgroup variants are deterministic");

  bool lodParity = true;
  for (uint32_t internalLOD = 0u; internalLOD < 4u; ++internalLOD)
  {
    const auto lodSource = fixture(internalLOD);
    const auto lodOutput = runVariant(rhi, lodSource, 128u, false);
    lodParity = lodParity && matchesCPUColumnGeneration(lodSource, lodOutput);
  }
  check(lodParity,
      "GPU fine bricks match CPU column occupancy and materials at LOD 0-3");

  const std::vector<Fixture> batchSources{
    fixture(0u), fixture(1u), fixture(2u), fixture(3u)};
  const auto batch = runBatch(rhi, batchSources);
  bool batchParity = batch.size() == batchSources.size();
  for (uint32_t lane = 0u; lane < batch.size(); ++lane)
  {
    const auto &actual = batch[lane];
    const auto &expected = batchSources[lane].expected;
    batchParity = batchParity &&
        actual.result.status == static_cast<uint32_t>(svr::TerrainGPUV2FineRasterStatus::Ready) &&
        actual.result.nodeCount == expected.result.nodeCount && actual.result.materialCount == expected.result.materialCount &&
        prefixEqual(actual.nodes, expected.nodes, actual.result.nodeCount) && prefixEqual(actual.occupancy, expected.occupancy, actual.result.nodeCount) &&
        prefixEqual(actual.materials, expected.materialWords, (actual.result.materialCount + 3u) / 4u) && prefixEqual(actual.roots, expected.rootIndices, actual.result.rootCount);
  }
  check(batchParity, "mixed four-lane GPU fine-raster outputs independently byte-match their CPU oracles");
}

void testWorldMapGPUFineRaster(
    ::rendering::backend::vulkan::VulkanRHI &rhi,
    const std::string &worldMapPath)
{
  std::cout << "real generated-world GPU fine raster\n";
  world::MapExportBundle source{};
  std::string error;
  check(world::readMapExportBundle(worldMapPath, source, &error),
        "default generated terrain bundle loads for raster test: " + error);
  if (!source.validate(&error))
  {
    check(false, "default generated terrain bundle validates: " + error);
    return;
  }
  world::MapExportGpuPageLayout layout;
  check(layout.initialize(source, 1u, &error),
        "real-map raster planning atlas initializes: " + error);
  svr::WorldMapTerrainSpawnView spawn{};
  check(svr::selectWorldMapTerrainSpawnView(
            source, source.maximumPageLevel, 0.5, 0.5, 16000.0,
            spawn, &error) && spawn.found,
        "raster test uses the renderer's automatic 16 km startup view: " +
            error);
  if (!spawn.found)
    return;

  // Match the renderer's 128x128 LOD-0 page, rather than proving the smaller
  // historical 64-cell fixture fits and then allowing the shipping page to be
  // truncated vertically.  Exercise both the camera page and the nearby slope
  // selected as its look target.
  for (const auto &point : {std::array<double, 2u>{spawn.spawnU, spawn.spawnV},
                            std::array<double, 2u>{spawn.targetU, spawn.targetV}})
  {
    const auto settings = worldColumnSettings(
        source, point[0], point[1], 0u, 128u);
    svr::TerrainGPUV2ColumnConfigGPU config{};
    std::vector<svr::TerrainGPUV2HeightSampleGPU> heights;
    std::vector<svr::TerrainGPUV2ColumnGPU> columns;
    const bool built = svr::planTerrainGPUV2Columns(
                           source, layout, settings, config, &error) &&
        svr::buildTerrainGPUV2ColumnOracle(
            source, config, heights, columns, &error);
    check(built, "renderer-sized close LOD0 slope columns build: " + error);
    if (!built)
      continue;
    const uint32_t required = requiredDynamicVerticalCells(
        columns, config.canonicalStep);
    std::cout << "    close LOD0 dynamic vertical cells=" << required
              << " capacity=128 at uv=" << point[0] << ',' << point[1]
              << '\n';
    check(required != 0u && required <= 128u,
          "renderer-sized close LOD0 slope fits the Tree64 vertical page "
          "without dropping upper wall voxels");
  }

  for (const uint32_t lod : {0u, 1u, 3u, 6u})
  {
    const auto fixtures = worldRasterFixtures(
        source, layout, spawn.spawnU, spawn.spawnV, lod);
    check(!fixtures.empty(),
          "real terrain surface intersects raster chunks at LOD " +
              std::to_string(lod));
    bool exact = !fixtures.empty();
    uint64_t occupied = 0u;
    for (const auto &fixture : fixtures)
    {
      const auto output = runVariant(rhi, fixture, 128u, false);
      exact &= output.result.status ==
                   static_cast<uint32_t>(
                       svr::TerrainGPUV2FineRasterStatus::Ready) &&
          output.result.nodeCount == fixture.expected.result.nodeCount &&
          output.result.materialCount == fixture.expected.result.materialCount &&
          output.result.occupiedVoxelCount ==
              fixture.expected.result.occupiedVoxelCount &&
          output.result.flags == fixture.expected.result.flags &&
          output.result.microSurfaceBytes == 0u &&
          output.result.perNodeVoxelScratchWords == 0u &&
          prefixEqual(
              output.nodes, fixture.expected.nodes,
              output.result.nodeCount) &&
          prefixEqual(
              output.occupancy, fixture.expected.occupancy,
              output.result.nodeCount) &&
          prefixEqual(
              output.materials, fixture.expected.materialWords,
              (output.result.materialCount + 3u) / 4u) &&
          matchesCPUColumnGeneration(fixture, output);
      occupied += output.result.occupiedVoxelCount;
    }
    check(exact && occupied != 0u,
          "real GPU LOD " + std::to_string(lod) +
              " emits byte-exact solid 8x8x8 occupancy/material bricks "
              "without microgeometry, overlaps, or missing surface voxels");
  }
}

} // namespace

int main(const int argc, const char *const *argv)
{
  os::Logger::start();
  ::rendering::DeviceRequiredLimits limits{.minimumMemory = 0u, .minimumComputeSharedMemory = 2048u, .minimumComputeWorkGroupInvocations = 256u};
  ::rendering::backend::vulkan::VulkanRHI rhi(::rendering::backend::vulkan::Vulkan_1_2, limits, ::rendering::DeviceFeatures::DeviceFeatures_Compute, {});
  std::vector<VkSurfaceKHR> surfaces;
  rhi.init(surfaces);
  testGPUFineRaster(rhi);
  if (argc >= 2)
    testWorldMapGPUFineRaster(rhi, argv[1]);
  else
    check(false, "real world-map path is required for GPU raster coverage");
  os::Logger::shutdown();
  std::cout << (failures == 0 ? "GPU terrain V2 Phase 4 tests passed\n" : "GPU terrain V2 Phase 4 tests failed\n");
  return failures == 0 ? 0 : 1;
}
