#include "os/Logger.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "voxel/svraster/TerrainGPUV2Tree64.hpp"
#include "voxel/svraster/SparseVoxelTerrainTree64.hpp"
#include "voxel/svraster/rendering/TerrainGPUV2Tree64Chain.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
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
  svr::TerrainGPUV2FineRasterResultGPU rasterResult{};
  std::vector<svr::SparseVoxelBrickNodeGPU> rasterNodes{};
  std::vector<svr::SparseVoxelBrickOccupancyGPU> rasterOccupancy{};
  svr::TerrainGPUV2Tree64ConfigGPU config{};
  svr::TerrainGPUV2Tree64Output expected{};
  svr::SparseVoxelTerrainPageTree64 cpuTree{};
};

Fixture fixture(const uint32_t internalLOD = 0u)
{
  Fixture value{};
  const uint32_t step = 1u << internalLOD;
  svr::TerrainGPUV2FineRasterConfigGPU raster{};
  // A 32x8x32 chunk pads to a 64-cubed model. Its 4,161 candidates force the
  // GPU scan through multiple level-0 blocks and exercise both prefix adds.
  raster.bricksX = 4u;
  raster.bricksY = 1u;
  raster.bricksZ = 4u;
  raster.brickSide = 8u;
  raster.denseBrickCount = 16u;
  raster.nodeCapacity = 16u;
  raster.materialCapacity = 8192u;
  raster.materialWordCapacity = 2048u;
  raster.canonicalStep = step;
  raster.voxelSizeMetres = 0.25f * static_cast<float>(step);
  raster.expectedTerrainGeneration = 19u;
  raster.regionMappingId = 7u;
  value.rasterNodes.resize(16u);
  value.rasterOccupancy.resize(16u);
  svr::WorldMapTerrainColumnPage page{};
  page.side = 32u;
  page.maximumVerticalVoxels = 8u;
  page.internalLOD = internalLOD;
  page.heightUnitMetres = 0.25 /
      svr::kWorldMapTerrainHeightUnitsPerVoxel;
  page.voxelSizeMetres = raster.voxelSizeMetres;
  page.edgeMetres = page.side * page.voxelSizeMetres;
  page.columns.resize(uint64_t{page.side} * page.side);
  page.walls.resize(page.columns.size());
  page.canonicalCornerHeights.resize(uint64_t{page.side + 1u} *
      (page.side + 1u));
  page.canonicalBoundaryMinimumHeights.resize(page.side * 4u);
  page.canonicalBoundaryMaximumHeights.resize(page.side * 4u);
  uint32_t prefix = 0u;
  for (uint32_t z = 0u; z < page.side; ++z)
    for (uint32_t x = 0u; x < page.side; ++x)
    {
      const uint32_t top = 2u + (x * 5u + z * 3u + x / 7u) % 5u;
      const uint32_t first = top - ((x + z * 2u) % 3u);
      page.columns[uint64_t{z} * page.side + x] = {top, first, 1u, 0u};
      page.walls[uint64_t{z} * page.side + x] = {first, first, 0u, 0u};
      const uint32_t bx = x / 8u;
      const uint32_t bz = z / 8u;
      const uint32_t nodeIndex = bx * 4u + bz;
      const uint32_t lx = x & 7u;
      const uint32_t lz = z & 7u;
      for (uint32_t y = first; y <= top; ++y)
      {
        const uint32_t cell = lx + 8u * y + 64u * lz;
        value.rasterOccupancy[nodeIndex].cells[cell / 32u].bits |=
            1u << (cell & 31u);
        ++prefix;
      }
    }
  for (uint32_t bx = 0u; bx < 4u; ++bx)
    for (uint32_t bz = 0u; bz < 4u; ++bz)
    {
      const uint32_t nodeIndex = bx * 4u + bz;
      auto &node = value.rasterNodes[nodeIndex];
      node.occupancyIndex = nodeIndex;
      node.canonicalGridOriginScale = {
        static_cast<int32_t>(bx * 8u * step), 0,
        static_cast<int32_t>(bz * 8u * step), static_cast<int32_t>(step)};
      uint32_t wordPrefix = 0u;
      for (auto &word : value.rasterOccupancy[nodeIndex].cells)
      {
        word.prefix = wordPrefix;
        wordPrefix += static_cast<uint32_t>(__builtin_popcount(word.bits));
      }
    }
  std::string error;
  check(svr::buildSparseVoxelTerrainPageTree64FromColumns(
            page, value.cpuTree, &error),
      "production CPU Tree64 fixture builds: " + error);
  value.rasterResult.status = static_cast<uint32_t>(svr::TerrainGPUV2FineRasterStatus::Ready);
  value.rasterResult.nodeCount = 16u;
  value.rasterResult.rootCount = 16u;
  value.rasterResult.occupiedVoxelCount = prefix;
  value.rasterResult.materialCount = prefix;
  value.rasterResult.terrainGeneration = 19u;
  svr::TerrainGPUV2Tree64Settings settings{};
  settings.capacity = {4161u, 4161u};
  settings.terrainSlot = 6u;
  settings.nodePageSlot = 12u;
  settings.nodePageGeneration = 8u;
  settings.geometryRevision = 0xaabbccddeeff0011ull;
  check(svr::planTerrainGPUV2Tree64(raster, settings, value.config, &error), "GPU Tree64 fixture plans: " + error);
  check(svr::buildTerrainGPUV2Tree64Oracle(value.config, value.rasterResult, value.rasterNodes, value.rasterOccupancy, value.expected, &error), "GPU Tree64 CPU oracle builds: " + error);
  return value;
}

struct GPUOutput
{
  svr::TerrainGPUV2Tree64ResultGPU result{};
  std::vector<svr::SparseVoxelTree64NodeGPU> nodes{};
};

GPUOutput runVariant(::rendering::backend::vulkan::VulkanRHI &rhi, const Fixture &source, const uint32_t workgroup, const bool failuresTest)
{
  using namespace ::rendering;
  RenderGraph graph(&rhi);
  const auto input = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push;
  const Buffer rasterNodes = graph.createBuffer({.name = "TerrainGPUV2.P5.RasterNodes", .size = source.rasterNodes.size() * sizeof(svr::SparseVoxelBrickNodeGPU), .usage = input});
  const Buffer rasterOccupancy = graph.createBuffer({.name = "TerrainGPUV2.P5.RasterOccupancy", .size = source.rasterOccupancy.size() * sizeof(svr::SparseVoxelBrickOccupancyGPU), .usage = input});
  const Buffer rasterResult = graph.createBuffer({.name = "TerrainGPUV2.P5.RasterResult", .size = sizeof(svr::TerrainGPUV2FineRasterResultGPU), .usage = input});
  graph.bufferWrite(rasterNodes, 0u, source.rasterNodes.size() * sizeof(source.rasterNodes[0]), const_cast<svr::SparseVoxelBrickNodeGPU *>(source.rasterNodes.data()));
  graph.bufferWrite(rasterOccupancy, 0u, source.rasterOccupancy.size() * sizeof(source.rasterOccupancy[0]), const_cast<svr::SparseVoxelBrickOccupancyGPU *>(source.rasterOccupancy.data()));
  graph.bufferWrite(rasterResult, 0u, sizeof(source.rasterResult), const_cast<svr::TerrainGPUV2FineRasterResultGPU *>(&source.rasterResult));
  const auto pull = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst;
  const Buffer resultReadback = graph.createBuffer({.name = "TerrainGPUV2.P5.ResultReadback", .size = sizeof(svr::TerrainGPUV2Tree64ResultGPU), .usage = pull});
  const Buffer nodeReadback = graph.createBuffer({.name = "TerrainGPUV2.P5.NodeReadback", .size = uint64_t{source.config.nodeCapacity} * sizeof(svr::SparseVoxelTree64NodeGPU), .usage = pull});
  uint32_t passIndex = 0u;
  GPUOutput output{};
  {
    svrRendering::TerrainGPUV2Tree64Chain chain(graph, passIndex, {rasterNodes, rasterOccupancy, rasterResult}, source.config, workgroup);
    auto *copyResult =
        graph.registerPass<gpgpu::CopyBufferPass>("TerrainGPUV2.P5.CopyResult", passIndex++, chain.result(), 0u, sizeof(svr::TerrainGPUV2Tree64ResultGPU), resultReadback, 0u, sizeof(svr::TerrainGPUV2Tree64ResultGPU));
    auto *copyNodes = graph.registerPass<gpgpu::CopyBufferPass>(
        "TerrainGPUV2.P5.CopyNodes",
        passIndex++,
        chain.nodes(),
        0u,
        uint64_t{source.config.nodeCapacity} * sizeof(svr::SparseVoxelTree64NodeGPU),
        nodeReadback,
        0u,
        uint64_t{source.config.nodeCapacity} * sizeof(svr::SparseVoxelTree64NodeGPU));
    check(chain.update(source.config), "GPU Tree64 chain accepts the planned config");
    check(chain.passCount() == 17u && chain.maskBytes() != 0u && chain.scratchBytes() != 0u && chain.outputBytes() != 0u, "GPU Tree64 chain exposes exact build memory and bounded dispatches");
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
      captured.result = read<svr::TerrainGPUV2Tree64ResultGPU>(graph, resultReadback, 1u).front();
      captured.nodes = read<svr::SparseVoxelTree64NodeGPU>(graph, nodeReadback, source.config.nodeCapacity);
      return captured;
    };
    run();
    output = capture();
    if (failuresTest)
    {
      const auto prior = output;
      auto stale = source.rasterResult;
      ++stale.terrainGeneration;
      graph.bufferWrite(rasterResult, 0u, sizeof(stale), &stale);
      run();
      const auto rejected = capture();
      check(
          rejected.result.status == static_cast<uint32_t>(svr::TerrainGPUV2Tree64Status::StaleRaster) &&
              std::memcmp(rejected.nodes.data(), prior.nodes.data(), uint64_t{prior.result.nodeCount} * sizeof(prior.nodes[0])) == 0,
          "stale raster rejection leaves the inactive Tree64 payload untouched");
      graph.bufferWrite(rasterResult, 0u, sizeof(source.rasterResult), const_cast<svr::TerrainGPUV2FineRasterResultGPU *>(&source.rasterResult));
      auto overflow = source.config;
      overflow.nodeCapacity = 1u;
      check(chain.update(overflow), "GPU Tree64 chain accepts a smaller per-job node budget");
      run();
      const auto overflowed = capture();
      check(
          overflowed.result.status == static_cast<uint32_t>(svr::TerrainGPUV2Tree64Status::Overflow) && overflowed.result.overflowStage == static_cast<uint32_t>(svr::TerrainGPUV2Tree64OverflowStage::Nodes) &&
              std::memcmp(overflowed.nodes.data(), prior.nodes.data(), uint64_t{prior.result.nodeCount} * sizeof(prior.nodes[0])) == 0,
          "GPU node overflow is rejected before compact output modification");
    }
    delete copyNodes;
    delete copyResult;
  }
  graph.deleteBuffer(nodeReadback);
  graph.deleteBuffer(resultReadback);
  graph.deleteBuffer(rasterResult);
  graph.deleteBuffer(rasterOccupancy);
  graph.deleteBuffer(rasterNodes);
  return output;
}

std::vector<GPUOutput> runBatch(
    ::rendering::backend::vulkan::VulkanRHI &rhi,
    const std::vector<Fixture> &sources)
{
  using namespace ::rendering;
  const uint32_t lanes = static_cast<uint32_t>(sources.size());
  const auto &capacity = sources.front().config;
  std::vector<svr::SparseVoxelBrickNodeGPU> rasterNodes(uint64_t{lanes} * capacity.rasterNodeCapacity);
  std::vector<svr::SparseVoxelBrickOccupancyGPU> rasterOccupancy(uint64_t{lanes} * capacity.rasterNodeCapacity);
  std::vector<svr::TerrainGPUV2FineRasterResultGPU> rasterResults(lanes);
  std::vector<svr::TerrainGPUV2Tree64ConfigGPU> configs;
  for (uint32_t lane = 0u; lane < lanes; ++lane)
  {
    std::copy(sources[lane].rasterNodes.begin(), sources[lane].rasterNodes.end(), rasterNodes.begin() + uint64_t{lane} * capacity.rasterNodeCapacity);
    std::copy(sources[lane].rasterOccupancy.begin(), sources[lane].rasterOccupancy.end(), rasterOccupancy.begin() + uint64_t{lane} * capacity.rasterNodeCapacity);
    rasterResults[lane] = sources[lane].rasterResult;
    configs.push_back(sources[lane].config);
  }
  RenderGraph graph(&rhi);
  const auto input = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push;
  const auto pull = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst;
  const Buffer rasterNodeBuffer = graph.createBuffer({.name = "TerrainGPUV2.P5.BatchRasterNodes", .size = rasterNodes.size() * sizeof(rasterNodes[0]), .usage = input});
  const Buffer rasterOccupancyBuffer = graph.createBuffer({.name = "TerrainGPUV2.P5.BatchRasterOccupancy", .size = rasterOccupancy.size() * sizeof(rasterOccupancy[0]), .usage = input});
  const Buffer rasterResultBuffer = graph.createBuffer({.name = "TerrainGPUV2.P5.BatchRasterResults", .size = rasterResults.size() * sizeof(rasterResults[0]), .usage = input});
  graph.bufferWrite(rasterNodeBuffer, 0u, rasterNodes.size() * sizeof(rasterNodes[0]), rasterNodes.data());
  graph.bufferWrite(rasterOccupancyBuffer, 0u, rasterOccupancy.size() * sizeof(rasterOccupancy[0]), rasterOccupancy.data());
  graph.bufferWrite(rasterResultBuffer, 0u, rasterResults.size() * sizeof(rasterResults[0]), rasterResults.data());
  const uint64_t resultBytes = uint64_t{lanes} * sizeof(svr::TerrainGPUV2Tree64ResultGPU);
  const uint64_t nodeBytes = uint64_t{lanes} * capacity.nodeCapacity * sizeof(svr::SparseVoxelTree64NodeGPU);
  const Buffer resultReadback = graph.createBuffer({.name = "TerrainGPUV2.P5.BatchResults", .size = resultBytes, .usage = pull});
  const Buffer nodeReadback = graph.createBuffer({.name = "TerrainGPUV2.P5.BatchNodes", .size = nodeBytes, .usage = pull});
  uint32_t passIndex = 0u;
  std::vector<GPUOutput> outputs(lanes);
  {
    svrRendering::TerrainGPUV2Tree64Chain chain(graph, passIndex, {rasterNodeBuffer, rasterOccupancyBuffer, rasterResultBuffer}, capacity, 128u, lanes, "TerrainGPUV2.P5.BatchTree64");
    auto *copyResult = graph.registerPass<gpgpu::CopyBufferPass>("TerrainGPUV2.P5.BatchCopyResults", passIndex++, chain.result(), 0u, resultBytes, resultReadback, 0u, resultBytes);
    auto *copyNodes = graph.registerPass<gpgpu::CopyBufferPass>("TerrainGPUV2.P5.BatchCopyNodes", passIndex++, chain.nodes(), 0u, nodeBytes, nodeReadback, 0u, nodeBytes);
    check(chain.updateBatch(configs), "mixed Tree64 batch accepts every active lane");
    check(std::all_of(chain.indirectDispatchPlan().begin(), chain.indirectDispatchPlan().end(), [lanes](const auto &dispatch) { return dispatch.z == lanes; }),
        "every Tree64 stage dispatches the active lane count indirectly");
    graph.compile();
    RenderGraph::Frame frame{};
    RenderGraph::Overrides overrides{};
    graph.run(frame, overrides);
    graph.waitFrame(frame);
    const auto results = read<svr::TerrainGPUV2Tree64ResultGPU>(graph, resultReadback, lanes);
    const auto nodes = read<svr::SparseVoxelTree64NodeGPU>(graph, nodeReadback, lanes * capacity.nodeCapacity);
    for (uint32_t lane = 0u; lane < lanes; ++lane)
    {
      outputs[lane].result = results[lane];
      outputs[lane].nodes.assign(nodes.begin() + uint64_t{lane} * capacity.nodeCapacity, nodes.begin() + uint64_t{lane + 1u} * capacity.nodeCapacity);
    }
    delete copyNodes;
    delete copyResult;
  }
  graph.deleteBuffer(nodeReadback); graph.deleteBuffer(resultReadback); graph.deleteBuffer(rasterResultBuffer);
  graph.deleteBuffer(rasterOccupancyBuffer); graph.deleteBuffer(rasterNodeBuffer);
  return outputs;
}

bool matches(const Fixture &source, const GPUOutput &actual)
{
  return actual.result.status == static_cast<uint32_t>(svr::TerrainGPUV2Tree64Status::Ready) && actual.result.nodeCount == source.expected.result.nodeCount &&
         actual.result.occupiedVoxelCount == source.expected.result.occupiedVoxelCount && actual.result.validationErrors == 0u && actual.result.fullPayloadHashWords == 0u &&
         std::memcmp(&actual.result.descriptor, &source.expected.result.descriptor, sizeof(actual.result.descriptor)) == 0 &&
         std::memcmp(actual.nodes.data(), source.expected.nodes.data(), uint64_t{actual.result.nodeCount} * sizeof(actual.nodes[0])) == 0;
}

bool matchesProductionCPUTree(const Fixture &source, const GPUOutput &actual)
{
  if (actual.result.status !=
          static_cast<uint32_t>(svr::TerrainGPUV2Tree64Status::Ready) ||
      actual.result.descriptor.modelSide != source.cpuTree.side ||
      actual.result.occupiedVoxelCount != source.cpuTree.occupiedVoxelCount ||
      actual.result.nodeCount > actual.nodes.size())
    return false;
  svr::SparseVoxelTree64Chunk gpuTree{};
  gpuTree.side = actual.result.descriptor.modelSide;
  gpuTree.nodes.assign(actual.nodes.begin(),
      actual.nodes.begin() + actual.result.nodeCount);
  for (uint32_t z = 0u; z < gpuTree.side; ++z)
    for (uint32_t y = 0u; y < gpuTree.side; ++y)
      for (uint32_t x = 0u; x < gpuTree.side; ++x)
      {
        const std::array<uint32_t, 3u> cell{x, y, z};
        if (svr::sparseVoxelTree64Occupied(gpuTree, cell) !=
            svr::sparseVoxelTree64Occupied(source.cpuTree.tree, cell))
          return false;
      }
  return true;
}

void testGPU(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  const auto source = fixture();
  const auto a = runVariant(rhi, source, 64u, true);
  const auto b = runVariant(rhi, source, 128u, false);
  const auto c = runVariant(rhi, source, 256u, false);
  check(matches(source, a) && matches(source, b) && matches(source, c), "64/128/256-thread GPU Tree64 outputs byte-match the CPU oracle");
  check(
      std::memcmp(a.nodes.data(), b.nodes.data(), uint64_t{a.result.nodeCount} * sizeof(a.nodes[0])) == 0 && std::memcmp(a.nodes.data(), c.nodes.data(), uint64_t{a.result.nodeCount} * sizeof(a.nodes[0])) == 0,
      "Tree64 masks, scan, compaction, and links are deterministic across workgroups");

  bool lodParity = true;
  for (uint32_t internalLOD = 0u; internalLOD < 4u; ++internalLOD)
  {
    const auto lodSource = fixture(internalLOD);
    const auto lodOutput = runVariant(rhi, lodSource, 128u, false);
    lodParity = lodParity && matchesProductionCPUTree(lodSource, lodOutput);
  }
  check(lodParity,
      "GPU Tree64 matches production CPU Tree64 occupancy through every tree level at terrain LOD 0-3");

  const std::vector<Fixture> batchSources{
    fixture(0u), fixture(1u), fixture(2u), fixture(3u)};
  const auto batch = runBatch(rhi, batchSources);
  check(
      batch.size() == batchSources.size() && matches(batchSources[0], batch[0]) && matches(batchSources[1], batch[1]),
      "mixed four-lane GPU Tree64 outputs independently byte-match their CPU oracles");
}
} // namespace

int main()
{
  os::Logger::start();
  ::rendering::DeviceRequiredLimits limits{.minimumMemory = 0u, .minimumComputeSharedMemory = 2048u, .minimumComputeWorkGroupInvocations = 256u};
  ::rendering::backend::vulkan::VulkanRHI rhi(::rendering::backend::vulkan::Vulkan_1_2, limits, ::rendering::DeviceFeatures::DeviceFeatures_Compute, {});
  std::vector<VkSurfaceKHR> surfaces;
  rhi.init(surfaces);
  testGPU(rhi);
  os::Logger::shutdown();
  std::cout << (failures == 0 ? "GPU terrain V2 Phase 5 tests passed\n" : "GPU terrain V2 Phase 5 tests failed\n");
  return failures == 0 ? 0 : 1;
}
