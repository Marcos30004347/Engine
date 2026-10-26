#include "os/Logger.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "voxel/svraster/TerrainGPUV2RasterHierarchy.hpp"
#include "voxel/svraster/rendering/TerrainGPUV2FineRasterChain.hpp"
#include "voxel/svraster/rendering/TerrainGPUV2RasterHierarchyChain.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace svr = voxel::svraster;
namespace svrRendering = voxel::svraster::rendering;

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

struct Fixture
{
  svr::TerrainGPUV2FineRasterConfigGPU rasterConfig{};
  svr::TerrainGPUV2ColumnResultGPU columnResult{};
  std::vector<svr::TerrainGPUV2HeightSampleGPU> heights{};
  std::vector<svr::TerrainGPUV2ColumnGPU> columns{};
  svr::TerrainGPUV2FineRasterOutput raster{};
  svr::TerrainGPUV2RasterHierarchyConfigGPU config{};
  svr::TerrainGPUV2RasterHierarchyOutput expected{};
};

Fixture fixture(const uint32_t internalLOD = 0u)
{
  Fixture value{};
  const uint32_t step = 1u << internalLOD;
  constexpr uint32_t generation = 41u;
  const svr::TerrainGPUChunkShape shape{4u, 2u, 4u, 8u};
  const svr::TerrainGPUChunkKey key{
    71u, 0, 0, 0, static_cast<uint16_t>(internalLOD), 1u, 4u,
    generation};
  svr::TerrainGPUV2ColumnConfigGPU columnsConfig{};
  columnsConfig.columnsX = 32u;
  columnsConfig.columnsZ = 32u;
  columnsConfig.halo = 2u;
  columnsConfig.heightUnitMetres = 0.5f;
  columnsConfig.canonicalStep = step;
  columnsConfig.expectedTerrainGeneration = generation;
  svr::TerrainGPUV2FineRasterSettings rasterSettings{};
  rasterSettings.key = key;
  rasterSettings.shape = shape;
  rasterSettings.columns = columnsConfig;
  rasterSettings.capacity = {32u, 16384u};
  rasterSettings.finestVoxelSizeMetres = 0.5;
  rasterSettings.regionMappingId = 31u;
  rasterSettings.paletteBase = 3u;
  std::string error;
  check(svr::planTerrainGPUV2FineRaster(rasterSettings, value.rasterConfig, &error), "GPU hierarchy fine-raster input plans: " + error);
  const uint32_t stride = columnsConfig.columnsX + 2u * columnsConfig.halo;
  const uint32_t sampleStride = stride + 1u;
  value.heights.resize(uint64_t{sampleStride} *
      (columnsConfig.columnsZ + 2u * columnsConfig.halo + 1u));
  for (uint32_t z = 0u;
       z < columnsConfig.columnsZ + 2u * columnsConfig.halo + 1u;
       ++z)
    for (uint32_t x = 0u; x < sampleStride; ++x)
    {
      const int32_t localX = static_cast<int32_t>(x) -
          static_cast<int32_t>(columnsConfig.halo);
      const int32_t localZ = static_cast<int32_t>(z) -
          static_cast<int32_t>(columnsConfig.halo);
      const int32_t clampedX = std::max(localX, 0);
      const int32_t clampedZ = std::max(localZ, 0);
      const int32_t lower = 3 +
          static_cast<int32_t>((clampedX + clampedZ * 2u) % 7u);
      const int32_t heightUnits =
          lower * static_cast<int32_t>(step) +
          static_cast<int32_t>(
              internalLOD != 0u && ((clampedX ^ clampedZ) & 1u) != 0u
                  ? step / 2u
                  : 0u);
      const double residual = ((x * 3u + z) % 7u == 0u) ? 0.625 : 0.25;
      const double heightMetres =
          (static_cast<double>(heightUnits) - residual) *
          columnsConfig.heightUnitMetres;
      value.heights[uint64_t{z} * sampleStride + x] = {
          heightUnits,
          static_cast<int32_t>(
              value.rasterConfig.chunkMinimumCanonicalX +
              localX * static_cast<int32_t>(step)),
          static_cast<int32_t>(
              value.rasterConfig.chunkMinimumCanonicalZ +
              localZ * static_cast<int32_t>(step)),
          svr::terrainGPUV2PackHeightSampleFlags(
              svr::TerrainGPUV2ColumnValid,
              heightMetres,
              columnsConfig.heightUnitMetres,
              heightUnits)};
    }
  value.columns.resize(uint64_t{stride} *
      (columnsConfig.columnsZ + 2u * columnsConfig.halo));
  for (auto &column : value.columns)
    column = {0, 0, 0u, svr::TerrainGPUV2ColumnValid};
  for (uint32_t z = 0u; z < columnsConfig.columnsZ; ++z)
    for (uint32_t x = 0u; x < columnsConfig.columnsX; ++x)
    {
      auto &column = value.columns[uint64_t{z + columnsConfig.halo} * stride + x + columnsConfig.halo];
      const int32_t lower = 3 + static_cast<int32_t>((x + z * 2u) % 7u);
      column.minimumHeightUnits = lower * static_cast<int32_t>(step) +
          static_cast<int32_t>(internalLOD != 0u && ((x ^ z) & 1u) != 0u
              ? step / 2u : 0u);
      column.maximumHeightUnits = ((x + z) % 9u) == 0u
          ? column.minimumHeightUnits
          : column.minimumHeightUnits +
              static_cast<int32_t>((1u + (x * 5u + z * 9u) % 4u) * step);
      column.material = 1u + (x / 3u + z / 5u) % 23u;
    }
  value.columnResult.status = static_cast<uint32_t>(svr::TerrainGPUV2ColumnStatus::Ready);
  value.columnResult.terrainGeneration = generation;
  value.columnResult.columnCount = static_cast<uint32_t>(value.columns.size());
  value.columnResult.writtenColumns = value.columnResult.columnCount;
  check(svr::buildTerrainGPUV2FineRasterOracle(value.rasterConfig, value.columnResult, value.columns, value.raster, &error), "GPU hierarchy fine-raster oracle builds: " + error);
  svr::TerrainGPUV2RasterHierarchySettings hierarchy{};
  hierarchy.capacity = {37u, 18944u, 40u, 1u};
  check(svr::planTerrainGPUV2RasterHierarchy(value.rasterConfig, hierarchy, value.config, &error), "GPU hierarchy fixture plans: " + error);
  check(
      svr::buildTerrainGPUV2RasterHierarchyOracle(
          value.config, value.raster.result, value.raster.nodes, value.raster.occupancy, value.raster.materialWords, value.heights, value.expected, &error),
      "GPU hierarchy CPU oracle builds: " + error);
  return value;
}

struct GPUOutput
{
  svr::TerrainGPUV2RasterHierarchyResultGPU result{};
  std::vector<svr::SparseVoxelBrickNodeGPU> nodes{};
  std::vector<svr::SparseVoxelBrickOccupancyGPU> occupancy{};
  std::vector<uint32_t> materialWords{};
  std::vector<uint32_t> childIndices{};
  std::vector<uint32_t> roots{};
};

GPUOutput runVariant(::rendering::backend::vulkan::VulkanRHI &rhi, const Fixture &source, const uint32_t workgroup, const bool staleTest)
{
  using namespace ::rendering;
  RenderGraph graph(&rhi);
  const auto input = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push;
  const Buffer columns = graph.createBuffer(
      {.name = "TerrainGPUV2.P6.Columns",
       .size = source.columns.size() * sizeof(source.columns[0]),
       .usage = input});
  const Buffer columnResult = graph.createBuffer(
      {.name = "TerrainGPUV2.P6.ColumnResult",
       .size = sizeof(source.columnResult), .usage = input});
  const Buffer heights = graph.createBuffer(
      {.name = "TerrainGPUV2.P6.Heights",
       .size = source.heights.size() * sizeof(source.heights[0]),
       .usage = input});
  graph.bufferWrite(columns, 0u,
      source.columns.size() * sizeof(source.columns[0]),
      const_cast<svr::TerrainGPUV2ColumnGPU *>(source.columns.data()));
  graph.bufferWrite(columnResult, 0u, sizeof(source.columnResult),
      const_cast<svr::TerrainGPUV2ColumnResultGPU *>(&source.columnResult));
  graph.bufferWrite(heights, 0u,
      source.heights.size() * sizeof(source.heights[0]),
      const_cast<svr::TerrainGPUV2HeightSampleGPU *>(
          source.heights.data()));

  const auto pull = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst;
  const Buffer resultReadback = graph.createBuffer({.name = "TerrainGPUV2.P6.ResultReadback", .size = sizeof(svr::TerrainGPUV2RasterHierarchyResultGPU), .usage = pull});
  const Buffer nodesReadback = graph.createBuffer(
      {.name = "TerrainGPUV2.P6.NodesReadback", .size = uint64_t{source.config.nodeCapacity} * sizeof(svr::SparseVoxelBrickNodeGPU), .usage = pull});
  const Buffer occupancyReadback = graph.createBuffer(
      {.name = "TerrainGPUV2.P6.OccupancyReadback", .size = uint64_t{source.config.nodeCapacity} * sizeof(svr::SparseVoxelBrickOccupancyGPU), .usage = pull});
  const Buffer materialsReadback = graph.createBuffer(
      {.name = "TerrainGPUV2.P6.MaterialReadback", .size = uint64_t{source.config.materialWordCapacity} * sizeof(uint32_t), .usage = pull});
  const Buffer childrenReadback = graph.createBuffer(
      {.name = "TerrainGPUV2.P6.ChildrenReadback", .size = uint64_t{source.config.childCapacity} * sizeof(uint32_t), .usage = pull});
  const Buffer rootsReadback = graph.createBuffer(
      {.name = "TerrainGPUV2.P6.RootsReadback", .size = uint64_t{source.config.rootCapacity} * sizeof(uint32_t), .usage = pull});

  uint32_t passIndex = 0u;
  GPUOutput output{};
  {
    svrRendering::TerrainGPUV2FineRasterChain fine(
        graph, passIndex, {columns, columnResult}, source.rasterConfig,
        workgroup, "TerrainGPUV2.P6.DirectFine");
    svrRendering::TerrainGPUV2RasterHierarchyChain chain(
        graph, passIndex,
        {fine.nodes(), fine.occupancy(), fine.materialWords(), fine.result(),
            heights},
        source.config, workgroup);
    std::vector<Pass *> copies;
    auto copy = [&](const std::string &name, const Buffer sourceBuffer, const uint64_t bytes, const Buffer destination)
    {
      copies.push_back(graph.registerPass<gpgpu::CopyBufferPass>(name, passIndex++, sourceBuffer, 0u, bytes, destination, 0u, bytes));
    };
    copy("TerrainGPUV2.P6.CopyResult", chain.result(), sizeof(svr::TerrainGPUV2RasterHierarchyResultGPU), resultReadback);
    copy("TerrainGPUV2.P6.CopyNodes", chain.nodes(), uint64_t{source.config.nodeCapacity} * sizeof(svr::SparseVoxelBrickNodeGPU), nodesReadback);
    copy("TerrainGPUV2.P6.CopyOccupancy", chain.occupancy(), uint64_t{source.config.nodeCapacity} * sizeof(svr::SparseVoxelBrickOccupancyGPU), occupancyReadback);
    copy("TerrainGPUV2.P6.CopyMaterials", chain.materialWords(), uint64_t{source.config.materialWordCapacity} * sizeof(uint32_t), materialsReadback);
    copy("TerrainGPUV2.P6.CopyChildren", chain.childIndices(), uint64_t{source.config.childCapacity} * sizeof(uint32_t), childrenReadback);
    copy("TerrainGPUV2.P6.CopyRoots", chain.rootIndices(), uint64_t{source.config.rootCapacity} * sizeof(uint32_t), rootsReadback);
    check(fine.update(source.rasterConfig) && chain.update(source.config),
        "direct GPU fine-raster/hierarchy chain accepts its planned configs");
    check(
        chain.passCount() == 5u + 11u * (source.config.levelCount - 1u) + 3u && chain.scratchBytes() != 0u && chain.outputBytes() != 0u && chain.microGeometryBytes() == 0u,
        "GPU hierarchy exposes bounded level dispatches and zero microgeometry");
    const auto &dispatches = chain.indirectDispatchPlan();
    const bool compactUpperScans = dispatches.size() == chain.passCount() &&
        dispatches[7].x == 1u && dispatches[8].x == 0u &&
        dispatches[9].x == 0u && dispatches[10].x == 0u &&
        dispatches[11].x == 0u && dispatches[18].x == 1u &&
        dispatches[19].x == 0u && dispatches[20].x == 0u &&
        dispatches[21].x == 0u && dispatches[22].x == 0u;
    check(compactUpperScans,
        "singleton hierarchy scan levels consume zero indirect workgroups");
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
      GPUOutput captured{};
      captured.result = read<svr::TerrainGPUV2RasterHierarchyResultGPU>(graph, resultReadback, 1u).front();
      captured.nodes = read<svr::SparseVoxelBrickNodeGPU>(graph, nodesReadback, source.config.nodeCapacity);
      captured.occupancy = read<svr::SparseVoxelBrickOccupancyGPU>(graph, occupancyReadback, source.config.nodeCapacity);
      captured.materialWords = read<uint32_t>(graph, materialsReadback, source.config.materialWordCapacity);
      captured.childIndices = read<uint32_t>(graph, childrenReadback, source.config.childCapacity);
      captured.roots = read<uint32_t>(graph, rootsReadback, source.config.rootCapacity);
      return captured;
    };
    run();
    output = capture();
    if (staleTest)
    {
      auto stale = source.raster.result;
      ++stale.terrainGeneration;
      fine.setEnabled(false);
      graph.bufferWrite(fine.result(), 0u, sizeof(stale), &stale);
      run();
      const auto rejected = capture();
      check(
          rejected.result.status == static_cast<uint32_t>(svr::TerrainGPUV2RasterHierarchyStatus::StaleRaster) &&
              std::memcmp(rejected.nodes.data(), output.nodes.data(), uint64_t{output.result.nodeCount} * sizeof(output.nodes[0])) == 0 &&
              std::memcmp(rejected.occupancy.data(), output.occupancy.data(), uint64_t{output.result.nodeCount} * sizeof(output.occupancy[0])) == 0,
          "stale fine-raster generation is rejected without touching the prior hierarchy payload");
    }
    for (auto iterator = copies.rbegin(); iterator != copies.rend(); ++iterator)
      delete *iterator;
  }

  graph.deleteBuffer(rootsReadback);
  graph.deleteBuffer(childrenReadback);
  graph.deleteBuffer(materialsReadback);
  graph.deleteBuffer(occupancyReadback);
  graph.deleteBuffer(nodesReadback);
  graph.deleteBuffer(resultReadback);
  graph.deleteBuffer(columnResult);
  graph.deleteBuffer(heights);
  graph.deleteBuffer(columns);
  return output;
}

std::vector<GPUOutput> runBatch(
    ::rendering::backend::vulkan::VulkanRHI &rhi,
    const std::vector<Fixture> &sources)
{
  using namespace ::rendering;
  constexpr uint32_t workgroup = 128u;
  const uint32_t lanes = static_cast<uint32_t>(sources.size());
  const auto &capacity = sources.front().config;
  RenderGraph graph(&rhi);
  const auto input = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push;
  const auto pull = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst;
  std::vector<svr::SparseVoxelBrickNodeGPU> leafNodes(uint64_t{lanes} * capacity.leafNodeCapacity);
  std::vector<svr::SparseVoxelBrickOccupancyGPU> leafOccupancy(uint64_t{lanes} * capacity.leafNodeCapacity);
  std::vector<uint32_t> leafMaterials(uint64_t{lanes} * capacity.leafMaterialWordCapacity);
  const uint32_t halo = capacity.reserved & 0xffffu;
  const uint32_t heightStride = capacity.reserved >> 16u;
  const uint64_t heightCount = uint64_t{heightStride} *
      (uint64_t{capacity.bricksZ} * capacity.brickSide + 2u * halo + 1u);
  std::vector<svr::TerrainGPUV2HeightSampleGPU> heights(
      uint64_t{lanes} * heightCount);
  std::vector<svr::TerrainGPUV2FineRasterResultGPU> rasterResults(lanes);
  std::vector<svr::TerrainGPUV2RasterHierarchyConfigGPU> configs;
  for (uint32_t lane = 0u; lane < lanes; ++lane)
  {
    const auto &source = sources[lane];
    std::copy(source.raster.nodes.begin(), source.raster.nodes.end(), leafNodes.begin() + uint64_t{lane} * capacity.leafNodeCapacity);
    std::copy(source.raster.occupancy.begin(), source.raster.occupancy.end(), leafOccupancy.begin() + uint64_t{lane} * capacity.leafNodeCapacity);
    std::copy(source.raster.materialWords.begin(), source.raster.materialWords.end(), leafMaterials.begin() + uint64_t{lane} * capacity.leafMaterialWordCapacity);
    std::copy(source.heights.begin(), source.heights.end(),
        heights.begin() + uint64_t{lane} * heightCount);
    rasterResults[lane] = source.raster.result;
    configs.push_back(source.config);
  }
  const Buffer leafNodeBuffer = graph.createBuffer({.name = "TerrainGPUV2.P6.BatchLeafNodes", .size = leafNodes.size() * sizeof(leafNodes[0]), .usage = input});
  const Buffer leafOccupancyBuffer = graph.createBuffer({.name = "TerrainGPUV2.P6.BatchLeafOccupancy", .size = leafOccupancy.size() * sizeof(leafOccupancy[0]), .usage = input});
  const Buffer leafMaterialBuffer = graph.createBuffer({.name = "TerrainGPUV2.P6.BatchLeafMaterials", .size = leafMaterials.size() * sizeof(leafMaterials[0]), .usage = input});
  const Buffer rasterResultBuffer = graph.createBuffer({.name = "TerrainGPUV2.P6.BatchRasterResults", .size = rasterResults.size() * sizeof(rasterResults[0]), .usage = input});
  const Buffer heightBuffer = graph.createBuffer(
      {.name = "TerrainGPUV2.P6.BatchHeights",
       .size = heights.size() * sizeof(heights[0]),
       .usage = input});
  graph.bufferWrite(leafNodeBuffer, 0u, leafNodes.size() * sizeof(leafNodes[0]), leafNodes.data());
  graph.bufferWrite(leafOccupancyBuffer, 0u, leafOccupancy.size() * sizeof(leafOccupancy[0]), leafOccupancy.data());
  graph.bufferWrite(leafMaterialBuffer, 0u, leafMaterials.size() * sizeof(leafMaterials[0]), leafMaterials.data());
  graph.bufferWrite(rasterResultBuffer, 0u, rasterResults.size() * sizeof(rasterResults[0]), rasterResults.data());
  graph.bufferWrite(heightBuffer, 0u, heights.size() * sizeof(heights[0]),
      heights.data());

  const uint64_t resultBytes = uint64_t{lanes} * sizeof(svr::TerrainGPUV2RasterHierarchyResultGPU);
  const uint64_t nodeBytes = uint64_t{lanes} * capacity.nodeCapacity * sizeof(svr::SparseVoxelBrickNodeGPU);
  const uint64_t occupancyBytes = uint64_t{lanes} * capacity.nodeCapacity * sizeof(svr::SparseVoxelBrickOccupancyGPU);
  const uint64_t materialBytes = uint64_t{lanes} * capacity.materialWordCapacity * sizeof(uint32_t);
  const uint64_t childBytes = uint64_t{lanes} * capacity.childCapacity * sizeof(uint32_t);
  const uint64_t rootBytes = uint64_t{lanes} * capacity.rootCapacity * sizeof(uint32_t);
  const Buffer resultReadback = graph.createBuffer({.name = "TerrainGPUV2.P6.BatchResultReadback", .size = resultBytes, .usage = pull});
  const Buffer nodeReadback = graph.createBuffer({.name = "TerrainGPUV2.P6.BatchNodeReadback", .size = nodeBytes, .usage = pull});
  const Buffer occupancyReadback = graph.createBuffer({.name = "TerrainGPUV2.P6.BatchOccupancyReadback", .size = occupancyBytes, .usage = pull});
  const Buffer materialReadback = graph.createBuffer({.name = "TerrainGPUV2.P6.BatchMaterialReadback", .size = materialBytes, .usage = pull});
  const Buffer childReadback = graph.createBuffer({.name = "TerrainGPUV2.P6.BatchChildReadback", .size = childBytes, .usage = pull});
  const Buffer rootReadback = graph.createBuffer({.name = "TerrainGPUV2.P6.BatchRootReadback", .size = rootBytes, .usage = pull});
  uint32_t passIndex = 0u;
  std::vector<GPUOutput> outputs(lanes);
  {
    svrRendering::TerrainGPUV2RasterHierarchyChain chain(
        graph, passIndex,
        {leafNodeBuffer, leafOccupancyBuffer, leafMaterialBuffer, rasterResultBuffer,
            heightBuffer},
        capacity, workgroup, lanes, "TerrainGPUV2.P6.BatchHierarchy");
    std::vector<Pass *> copies;
    auto copy = [&](const std::string &name, const Buffer source, const uint64_t bytes, const Buffer destination)
    {
      copies.push_back(graph.registerPass<gpgpu::CopyBufferPass>(name, passIndex++, source, 0u, bytes, destination, 0u, bytes));
    };
    copy("TerrainGPUV2.P6.BatchCopyResults", chain.result(), resultBytes, resultReadback);
    copy("TerrainGPUV2.P6.BatchCopyNodes", chain.nodes(), nodeBytes, nodeReadback);
    copy("TerrainGPUV2.P6.BatchCopyOccupancy", chain.occupancy(), occupancyBytes, occupancyReadback);
    copy("TerrainGPUV2.P6.BatchCopyMaterials", chain.materialWords(), materialBytes, materialReadback);
    copy("TerrainGPUV2.P6.BatchCopyChildren", chain.childIndices(), childBytes, childReadback);
    copy("TerrainGPUV2.P6.BatchCopyRoots", chain.rootIndices(), rootBytes, rootReadback);
    check(chain.updateBatch(configs), "mixed hierarchy batch accepts every active lane");
    const bool laneDispatch = std::all_of(
        chain.indirectDispatchPlan().begin(), chain.indirectDispatchPlan().end(),
        [lanes](const auto &dispatch) { return dispatch.z == lanes; });
    check(laneDispatch, "every hierarchy stage dispatches the active lane count indirectly");
    graph.compile();
    RenderGraph::Frame frame{};
    RenderGraph::Overrides overrides{};
    graph.run(frame, overrides);
    graph.waitFrame(frame);
    const auto results = read<svr::TerrainGPUV2RasterHierarchyResultGPU>(graph, resultReadback, lanes);
    const auto nodes = read<svr::SparseVoxelBrickNodeGPU>(graph, nodeReadback, lanes * capacity.nodeCapacity);
    const auto occupancy = read<svr::SparseVoxelBrickOccupancyGPU>(graph, occupancyReadback, lanes * capacity.nodeCapacity);
    const auto materials = read<uint32_t>(graph, materialReadback, lanes * capacity.materialWordCapacity);
    const auto children = read<uint32_t>(graph, childReadback, lanes * capacity.childCapacity);
    const auto roots = read<uint32_t>(graph, rootReadback, lanes * capacity.rootCapacity);
    for (uint32_t lane = 0u; lane < lanes; ++lane)
    {
      auto &output = outputs[lane];
      output.result = results[lane];
      output.nodes.assign(nodes.begin() + uint64_t{lane} * capacity.nodeCapacity, nodes.begin() + uint64_t{lane + 1u} * capacity.nodeCapacity);
      output.occupancy.assign(occupancy.begin() + uint64_t{lane} * capacity.nodeCapacity, occupancy.begin() + uint64_t{lane + 1u} * capacity.nodeCapacity);
      output.materialWords.assign(materials.begin() + uint64_t{lane} * capacity.materialWordCapacity, materials.begin() + uint64_t{lane + 1u} * capacity.materialWordCapacity);
      output.childIndices.assign(children.begin() + uint64_t{lane} * capacity.childCapacity, children.begin() + uint64_t{lane + 1u} * capacity.childCapacity);
      output.roots.assign(roots.begin() + uint64_t{lane} * capacity.rootCapacity, roots.begin() + uint64_t{lane + 1u} * capacity.rootCapacity);
    }
    for (auto iterator = copies.rbegin(); iterator != copies.rend(); ++iterator)
      delete *iterator;
  }
  graph.deleteBuffer(rootReadback);
  graph.deleteBuffer(childReadback);
  graph.deleteBuffer(materialReadback);
  graph.deleteBuffer(occupancyReadback);
  graph.deleteBuffer(nodeReadback);
  graph.deleteBuffer(resultReadback);
  graph.deleteBuffer(rasterResultBuffer);
  graph.deleteBuffer(heightBuffer);
  graph.deleteBuffer(leafMaterialBuffer);
  graph.deleteBuffer(leafOccupancyBuffer);
  graph.deleteBuffer(leafNodeBuffer);
  return outputs;
}

bool matches(const Fixture &source, const GPUOutput &actual)
{
  const auto &expected = source.expected;
  return actual.result.status == static_cast<uint32_t>(svr::TerrainGPUV2RasterHierarchyStatus::Ready) && actual.result.validationErrors == 0u &&
         actual.result.leafNodeCount == expected.result.leafNodeCount && actual.result.nodeCount == expected.result.nodeCount &&
         actual.result.materialCount == expected.result.materialCount && actual.result.childCount == expected.result.childCount &&
         actual.result.rootCount == expected.result.rootCount && actual.result.terrainGeneration == expected.result.terrainGeneration &&
         actual.result.microSurfaceBytes == 0u && actual.result.selectedMode == static_cast<uint32_t>(svr::TerrainGPUV2RasterMode::Hierarchy) &&
         std::memcmp(actual.nodes.data(), expected.nodes.data(), uint64_t{expected.result.nodeCount} * sizeof(actual.nodes[0])) == 0 &&
         std::memcmp(actual.occupancy.data(), expected.occupancy.data(), uint64_t{expected.result.nodeCount} * sizeof(actual.occupancy[0])) == 0 &&
         std::memcmp(actual.materialWords.data(), expected.materialWords.data(), expected.materialWords.size() * sizeof(uint32_t)) == 0 &&
         std::memcmp(actual.childIndices.data(), expected.childIndices.data(), expected.childIndices.size() * sizeof(uint32_t)) == 0 &&
         std::memcmp(actual.roots.data(), expected.rootIndices.data(), expected.rootIndices.size() * sizeof(uint32_t)) == 0;
}

bool matchesCPUGeneratedHierarchy(const Fixture &source,
                                  const GPUOutput &actual)
{
  if (actual.result.status !=
          static_cast<uint32_t>(svr::TerrainGPUV2RasterHierarchyStatus::Ready) ||
      actual.result.levelCount != source.config.levelCount)
    return false;
  using CellSet = std::set<std::array<uint32_t, 3u>>;
  std::vector<CellSet> expected(source.config.levelCount);
  std::vector<CellSet> observed(source.config.levelCount);
  // Derive the independent hierarchy expectation from the compact fine-raster
  // payload. Reconstructing it from column min/max values duplicated the old
  // single-column rule and omitted neighbour cliff closure and boundary
  // stitching, producing a false hierarchy failure.
  for (uint32_t nodeIndex = 0u;
       nodeIndex < source.raster.result.nodeCount; ++nodeIndex)
  {
    const auto &node = source.raster.nodes[nodeIndex];
    if (node.occupancyIndex >= source.raster.occupancy.size())
      return false;
    std::array<uint32_t, 3u> base{};
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      const int32_t minimum = axis == 0u
          ? source.rasterConfig.chunkMinimumCanonicalX
          : axis == 1u ? source.rasterConfig.chunkMinimumCanonicalY
                       : source.rasterConfig.chunkMinimumCanonicalZ;
      const int32_t delta = node.canonicalGridOriginScale[axis] - minimum;
      if (delta < 0 ||
          delta % static_cast<int32_t>(source.rasterConfig.canonicalStep) != 0)
        return false;
      base[axis] = static_cast<uint32_t>(delta) /
          source.rasterConfig.canonicalStep;
    }
    const auto &mask = source.raster.occupancy[node.occupancyIndex];
    for (uint32_t word = 0u; word < 16u; ++word)
    {
      uint32_t bits = mask.cells[word].bits;
      while (bits != 0u)
      {
        const uint32_t bit = static_cast<uint32_t>(__builtin_ctz(bits));
        bits &= bits - 1u;
        const uint32_t cell = word * 32u + bit;
        const std::array<uint32_t, 3u> fine{
            base[0] + (cell & 7u),
            base[1] + ((cell >> 3u) & 7u),
            base[2] + (cell >> 6u)};
        for (uint32_t level = 0u; level < source.config.levelCount; ++level)
          expected[level].insert(
              {fine[0] >> level, fine[1] >> level, fine[2] >> level});
      }
    }
  }

  for (uint32_t nodeIndex = 0u; nodeIndex < actual.result.nodeCount;
       ++nodeIndex)
  {
    const auto &node = actual.nodes[nodeIndex];
    if (node.occupancyIndex >= actual.occupancy.size() ||
        node.canonicalGridOriginScale[3] <= 0)
      return false;
    uint32_t ratio = static_cast<uint32_t>(
        node.canonicalGridOriginScale[3]) /
        source.rasterConfig.canonicalStep;
    uint32_t level = 0u;
    while (ratio > 1u && (ratio & 1u) == 0u)
    {
      ratio >>= 1u;
      ++level;
    }
    if (ratio != 1u || level >= source.config.levelCount)
      return false;
    const int32_t cellStep = node.canonicalGridOriginScale[3];
    std::array<uint32_t, 3u> base{};
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      const int32_t minimum = axis == 0u
          ? source.config.chunkMinimumCanonicalX
          : axis == 1u ? source.config.chunkMinimumCanonicalY
                       : source.config.chunkMinimumCanonicalZ;
      const int32_t delta = node.canonicalGridOriginScale[axis] - minimum;
      if (delta < 0 || delta % cellStep != 0)
        return false;
      base[axis] = static_cast<uint32_t>(delta / cellStep);
    }
    const auto &mask = actual.occupancy[node.occupancyIndex];
    for (uint32_t word = 0u; word < 16u; ++word)
    {
      uint32_t bits = mask.cells[word].bits;
      while (bits != 0u)
      {
        const uint32_t bit = static_cast<uint32_t>(__builtin_ctz(bits));
        bits &= bits - 1u;
        const uint32_t cell = word * 32u + bit;
        if (!observed[level].insert({
              base[0] + (cell & 7u),
              base[1] + ((cell >> 3u) & 7u),
              base[2] + (cell >> 6u)}).second)
          return false;
      }
    }
  }
  return expected == observed;
}

void testGPU(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  const auto source = fixture();
  const auto a = runVariant(rhi, source, 64u, false);
  const auto b = runVariant(rhi, source, 128u, false);
  const auto c = runVariant(rhi, source, 256u, false);
  check(matches(source, a) && matches(source, b) && matches(source, c), "64/128/256-thread GPU hierarchies byte-match the CPU oracle");
  check(
      std::memcmp(a.nodes.data(), b.nodes.data(), uint64_t{a.result.nodeCount} * sizeof(a.nodes[0])) == 0 &&
          std::memcmp(a.nodes.data(), c.nodes.data(), uint64_t{a.result.nodeCount} * sizeof(a.nodes[0])) == 0,
      "GPU hierarchy occupancy, parent materials, errors, and links are deterministic across workgroups");

  bool lodParity = true;
  for (uint32_t internalLOD = 0u; internalLOD < 4u; ++internalLOD)
  {
    const auto lodSource = fixture(internalLOD);
    const auto lodOutput = runVariant(rhi, lodSource, 128u, false);
    lodParity = lodParity &&
        matchesCPUGeneratedHierarchy(lodSource, lodOutput);
  }
  check(lodParity,
      "every GPU brick hierarchy level matches CPU-generated occupancy at terrain LOD 0-3");

  const std::vector<Fixture> batchSources{
    fixture(0u), fixture(1u), fixture(2u), fixture(3u)};
  const auto batch = runBatch(rhi, batchSources);
  check(
      batch.size() == batchSources.size() && matches(batchSources[0], batch[0]) && matches(batchSources[1], batch[1]),
      "mixed four-lane GPU hierarchy outputs independently byte-match their CPU oracles");
}
} // namespace

int main()
{
  os::Logger::start();
  ::rendering::DeviceRequiredLimits limits{.minimumMemory = 0u, .minimumComputeSharedMemory = 4096u, .minimumComputeWorkGroupInvocations = 256u};
  ::rendering::backend::vulkan::VulkanRHI rhi(::rendering::backend::vulkan::Vulkan_1_2, limits, ::rendering::DeviceFeatures::DeviceFeatures_Compute, {});
  std::vector<VkSurfaceKHR> surfaces;
  rhi.init(surfaces);
  testGPU(rhi);
  os::Logger::shutdown();
  std::cout << (failures == 0 ? "GPU terrain V2 Phase 6 tests passed\n" : "GPU terrain V2 Phase 6 tests failed\n");
  return failures == 0 ? 0 : 1;
}
