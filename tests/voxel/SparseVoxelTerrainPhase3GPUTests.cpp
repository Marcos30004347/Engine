#include "os/Logger.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "voxel/svraster/rendering/TerrainGPURasterBuildChain.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#undef NDEBUG
#include <cassert>

namespace
{
using namespace ::rendering;
namespace svr = voxel::svraster;
namespace svrRendering = voxel::svraster::rendering;

svr::WorldMapTerrainColumnPage testPage()
{
  svr::WorldMapTerrainColumnPage page{};
  page.side = 8u;
  page.maximumVerticalVoxels = 32u;
  page.heightUnitMetres = 1.0 /
      svr::kWorldMapTerrainHeightUnitsPerVoxel;
  page.voxelSizeMetres = 0.5;
  page.edgeMetres = 4.0;
  page.elevationOriginMetres = -3.0;
  page.columns.resize(64u);
  page.walls.resize(64u);
  page.canonicalCornerHeights.resize(81u);
  page.canonicalBoundaryMinimumHeights.resize(32u);
  page.canonicalBoundaryMaximumHeights.resize(32u);
  for (uint32_t z = 0u; z < 8u; ++z)
    for (uint32_t x = 0u; x < 8u; ++x)
    {
      const uint32_t index = z * 8u + x;
      const uint32_t top = std::min(31u, 3u + x * 2u + z);
      const uint32_t first = top > 5u ? top - (2u + (x + z) % 4u) : 0u;
      const uint32_t unstitched = std::min(top, first + (x == 0u ? 2u : 0u));
      page.columns[index] = {
        top, first,
        ((x + z) % 6u) |
            (static_cast<uint32_t>(svr::SparseVoxelMicroContext::FlatSurface)
                << 8u),
        unstitched != first
            ? svr::WorldMapTerrainColumnBoundaryStitched : 0u,
      };
      page.walls[index] = {
        unstitched, top > 3u ? top - 3u : 0u,
        static_cast<uint32_t>(svr::SparseVoxelMicroContext::WallNegativeX) |
            (static_cast<uint32_t>(
                svr::SparseVoxelMicroContext::WallPositiveZ) << 8u),
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

template <typename T>
std::vector<T> readVector(RenderGraph &graph, const Buffer buffer,
    const size_t count)
{
  std::vector<T> values(count);
  graph.bufferRead(buffer, 0u, values.size() * sizeof(T), [&](const void *data)
  {
    std::memcpy(values.data(), data, values.size() * sizeof(T));
  });
  return values;
}

struct Readbacks
{
  Buffer nodes;
  Buffer occupancy;
  Buffer materialWords;
  Buffer micro;
  Buffer children;
  Buffer roots;
  Buffer result;
};

svr::TerrainGPURasterResultGPU runResultOnly(
    ::rendering::backend::vulkan::VulkanRHI &rhi,
    const svr::WorldMapTerrainColumnPage &page,
    const svr::SparseVoxelBrickHierarchy &expected,
    const svr::TerrainGPURasterCapacity capacity)
{
  svr::TerrainGPURasterConfigGPU config{};
  std::string error;
  assert(svr::planTerrainGPURasterBuild(page, capacity, 31u, 77u,
      static_cast<uint32_t>(expected.palette.size()), config, &error));
  std::array<svr::TerrainGPURasterMaterialGPU,
      svr::kTerrainGPURasterMaterialTableSize> table{};
  assert(svr::buildTerrainGPURasterMaterialTable(
      page, expected, table, &error));
  RenderGraph graph(&rhi);
  const Buffer readback = graph.createBuffer({
    .name = "Phase3.Overflow.Result",
    .size = sizeof(svr::TerrainGPURasterResultGPU),
    .usage = BufferUsage::BufferUsage_Pull |
        BufferUsage::BufferUsage_CopyDst,
  });
  uint32_t passIndex = 0u;
  svr::TerrainGPURasterResultGPU result{};
  {
    svrRendering::TerrainGPURasterBuildChain chain(
        graph, passIndex, capacity, static_cast<uint32_t>(page.columns.size()),
        "TerrainGPUBuild.Phase3.Overflow");
    assert(chain.update(page, config, table));
    auto *copy = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
        "Phase3.Overflow.Copy", passIndex++, chain.result(), 0u,
        sizeof(result), readback, 0u, sizeof(result));
    graph.compile();
    RenderGraph::Frame frame{};
    RenderGraph::Overrides overrides{};
    graph.run(frame, overrides);
    graph.waitFrame(frame);
    result = readVector<svr::TerrainGPURasterResultGPU>(
        graph, readback, 1u).front();
    delete copy;
  }
  graph.deleteBuffer(readback);
  return result;
}

void testOneElementOverflows(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  const auto page = testPage();
  const auto expected = reference(page);
  const svr::TerrainGPURasterCapacity exact{
    64u,
    static_cast<uint32_t>(expected.nodes.size()),
    static_cast<uint32_t>(expected.childIndices.size()),
    static_cast<uint32_t>(expected.materialIndices.size()),
    static_cast<uint32_t>(expected.rootIndices.size()),
  };
  struct Case
  {
    svr::TerrainGPURasterCapacity capacity;
    svr::TerrainGPURasterOverflowStage stage;
  };
  std::vector<Case> cases;
  auto capacity = exact;
  --capacity.nodes;
  cases.push_back({capacity, svr::TerrainGPURasterOverflowStage::Nodes});
  capacity = exact;
  --capacity.children;
  cases.push_back({capacity, svr::TerrainGPURasterOverflowStage::Children});
  capacity = exact;
  --capacity.materials;
  cases.push_back({capacity, svr::TerrainGPURasterOverflowStage::Materials});
  capacity = exact;
  --capacity.roots;
  cases.push_back({capacity, svr::TerrainGPURasterOverflowStage::Roots});
  for (const auto &item : cases)
  {
    const auto result = runResultOnly(rhi, page, expected, item.capacity);
    assert(result.status ==
        static_cast<uint32_t>(svr::TerrainGPURasterStatus::Overflow));
    assert(result.overflowStage == static_cast<uint32_t>(item.stage));
  }
}

void testDifferential(::rendering::backend::vulkan::VulkanRHI &rhi,
    const bool splitAcrossFrames)
{
  const auto page = testPage();
  const auto expected = reference(page);
  const svr::TerrainGPURasterCapacity capacity{
    64u,
    static_cast<uint32_t>(expected.nodes.size()),
    static_cast<uint32_t>(expected.childIndices.size()),
    static_cast<uint32_t>(expected.materialIndices.size()),
    static_cast<uint32_t>(expected.rootIndices.size()),
  };
  svr::TerrainGPURasterConfigGPU config{};
  std::string error;
  assert(svr::planTerrainGPURasterBuild(page, capacity, 23u,
      0xfedcba9876543210ull,
      static_cast<uint32_t>(expected.palette.size()), config, &error));
  std::array<svr::TerrainGPURasterMaterialGPU,
      svr::kTerrainGPURasterMaterialTableSize> table{};
  assert(svr::buildTerrainGPURasterMaterialTable(
      page, expected, table, &error));

  RenderGraph graph(&rhi);
  const uint64_t materialWordBytes =
      ((uint64_t{capacity.materials} + 3u) / 4u) * sizeof(uint32_t);
  const auto pull = BufferUsage::BufferUsage_Pull |
      BufferUsage::BufferUsage_CopyDst;
  Readbacks readback{
    graph.createBuffer({.name = "Phase3.Nodes.Readback",
      .size = uint64_t{capacity.nodes} * sizeof(svr::SparseVoxelBrickNodeGPU),
      .usage = pull}),
    graph.createBuffer({.name = "Phase3.Occupancy.Readback",
      .size = uint64_t{capacity.nodes} *
          sizeof(svr::SparseVoxelBrickOccupancyGPU), .usage = pull}),
    graph.createBuffer({.name = "Phase3.Material.Readback",
      .size = materialWordBytes, .usage = pull}),
    graph.createBuffer({.name = "Phase3.Micro.Readback",
      .size = uint64_t{capacity.materials} * sizeof(uint64_t), .usage = pull}),
    graph.createBuffer({.name = "Phase3.Children.Readback",
      .size = uint64_t{capacity.children} * sizeof(uint32_t), .usage = pull}),
    graph.createBuffer({.name = "Phase3.Roots.Readback",
      .size = uint64_t{capacity.roots} * sizeof(uint32_t), .usage = pull}),
    graph.createBuffer({.name = "Phase3.Result.Readback",
      .size = sizeof(svr::TerrainGPURasterResultGPU), .usage = pull}),
  };
  uint32_t passIndex = 0u;
  {
    svrRendering::TerrainGPURasterBuildChain chain(
        graph, passIndex, capacity, static_cast<uint32_t>(page.columns.size()));
    const auto copy = [&](const std::string &name, const Buffer source,
                          const uint64_t bytes, const Buffer destination)
    {
      return graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
          name, passIndex++, source, 0u, bytes, destination, 0u, bytes);
    };
    std::vector<::rendering::Pass *> copies{
      copy("Phase3.Copy.Nodes", chain.nodes(),
          uint64_t{capacity.nodes} * sizeof(svr::SparseVoxelBrickNodeGPU),
          readback.nodes),
      copy("Phase3.Copy.Occupancy", chain.occupancy(),
          uint64_t{capacity.nodes} *
              sizeof(svr::SparseVoxelBrickOccupancyGPU), readback.occupancy),
      copy("Phase3.Copy.Material", chain.materialWords(), materialWordBytes,
          readback.materialWords),
      copy("Phase3.Copy.Micro", chain.microSurfaceData(),
          uint64_t{capacity.materials} * sizeof(uint64_t), readback.micro),
      copy("Phase3.Copy.Children", chain.childIndices(),
          uint64_t{capacity.children} * sizeof(uint32_t), readback.children),
      copy("Phase3.Copy.Roots", chain.rootIndices(),
          uint64_t{capacity.roots} * sizeof(uint32_t), readback.roots),
      copy("Phase3.Copy.Result", chain.result(),
          sizeof(svr::TerrainGPURasterResultGPU), readback.result),
    };
    graph.compile();
    RenderGraph::Frame frame{};
    RenderGraph::Overrides overrides{};
    if (!splitAcrossFrames)
    {
      assert(chain.update(page, config, table));
      graph.run(frame, overrides);
      graph.waitFrame(frame);
    }
    else
    {
      for (uint32_t stage = 0u;
           stage < svrRendering::TerrainGPURasterBuildChain::kPassCount;
           ++stage)
      {
        assert(chain.update(page, config, table,
            static_cast<uint16_t>(1u << stage)));
        graph.run(frame, overrides);
        graph.waitFrame(frame);
      }
    }

    const auto gpuResult = readVector<svr::TerrainGPURasterResultGPU>(
        graph, readback.result, 1u).front();
    assert(gpuResult.status ==
        static_cast<uint32_t>(svr::TerrainGPURasterStatus::Ready));
    assert(gpuResult.validationErrors == 0u);
    assert(gpuResult.terrainGeneration == 23u);
    assert(gpuResult.nodeCount == expected.nodes.size());
    assert(gpuResult.childCount == expected.childIndices.size());
    assert(gpuResult.materialCount == expected.materialIndices.size());
    assert(gpuResult.rootCount == expected.rootIndices.size());

    svr::SparseVoxelBrickHierarchy actual{};
    actual.nodes = readVector<svr::SparseVoxelBrickNodeGPU>(
        graph, readback.nodes, gpuResult.nodeCount);
    actual.occupancy = readVector<svr::SparseVoxelBrickOccupancyGPU>(
        graph, readback.occupancy, gpuResult.nodeCount);
    const auto words = readVector<uint32_t>(graph, readback.materialWords,
        (gpuResult.materialCount + 3u) / 4u);
    actual.materialIndices.resize(gpuResult.materialCount);
    for (uint32_t index = 0u; index < gpuResult.materialCount; ++index)
      actual.materialIndices[index] = static_cast<uint8_t>(
          (words[index / 4u] >> ((index & 3u) * 8u)) & 0xffu);
    actual.microSurfaceData = readVector<uint64_t>(
        graph, readback.micro, gpuResult.materialCount);
    actual.childIndices = readVector<uint32_t>(
        graph, readback.children, gpuResult.childCount);
    actual.rootIndices = readVector<uint32_t>(
        graph, readback.roots, gpuResult.rootCount);
    actual.palette = expected.palette;
    actual.paletteDescriptorIds = expected.paletteDescriptorIds;
    actual.paletteStride = expected.paletteStride;
    actual.rootCount = gpuResult.rootCount;
    actual.levelCount = gpuResult.levelCount;
    actual.finestVoxelSize = static_cast<float>(page.voxelSizeMetres);

    assert(svr::validateTerrainGPURasterHierarchy(actual, &error));
    assert(actual.nodes.size() == expected.nodes.size());
    if (std::memcmp(actual.nodes.data(), expected.nodes.data(),
        actual.nodes.size() * sizeof(actual.nodes.front())) != 0)
    {
      for (size_t index = 0u; index < actual.nodes.size(); ++index)
      {
        const auto &left = expected.nodes[index];
        const auto &right = actual.nodes[index];
        if (std::memcmp(&left, &right, sizeof(left)) == 0) continue;
        std::cerr << "node mismatch " << index
                  << " expected origin " << left.canonicalGridOriginScale[0]
                  << ',' << left.canonicalGridOriginScale[1] << ','
                  << left.canonicalGridOriginScale[2] << ','
                  << left.canonicalGridOriginScale[3]
                  << " actual origin " << right.canonicalGridOriginScale[0]
                  << ',' << right.canonicalGridOriginScale[1] << ','
                  << right.canonicalGridOriginScale[2] << ','
                  << right.canonicalGridOriginScale[3]
                  << " material " << left.materialBase << '/'
                  << right.materialBase << " children " << left.firstChild
                  << ',' << left.childCount << '/' << right.firstChild << ','
                  << right.childCount << " error " << left.selfError << ','
                  << left.parentError << '/' << right.selfError << ','
                  << right.parentError << '\n';
        break;
      }
      assert(false);
    }
    assert(std::memcmp(actual.occupancy.data(), expected.occupancy.data(),
        actual.occupancy.size() * sizeof(actual.occupancy.front())) == 0);
    assert(actual.materialIndices == expected.materialIndices);
    assert(actual.microSurfaceData == expected.microSurfaceData);
    assert(actual.childIndices == expected.childIndices);
    assert(actual.rootIndices == expected.rootIndices);
    const uint64_t hash = svr::terrainGPURasterSemanticHash(actual);
    assert(hash == svr::terrainGPURasterSemanticHash(expected));
    assert(gpuResult.semanticHashLow == static_cast<uint32_t>(hash));
    assert(gpuResult.semanticHashHigh == static_cast<uint32_t>(hash >> 32u));
    for (auto iterator = copies.rbegin(); iterator != copies.rend(); ++iterator)
      delete *iterator;
  }
  graph.deleteBuffer(readback.result);
  graph.deleteBuffer(readback.roots);
  graph.deleteBuffer(readback.children);
  graph.deleteBuffer(readback.micro);
  graph.deleteBuffer(readback.materialWords);
  graph.deleteBuffer(readback.occupancy);
  graph.deleteBuffer(readback.nodes);
}
} // namespace

int main()
{
  os::Logger::start();
  ::rendering::DeviceRequiredLimits limits{
    .minimumMemory = 0u,
    .minimumComputeSharedMemory = 0u,
    .minimumComputeWorkGroupInvocations = 1u,
  };
  ::rendering::backend::vulkan::VulkanRHI rhi(
      ::rendering::backend::vulkan::Vulkan_1_2, limits,
      ::rendering::DeviceFeatures::DeviceFeatures_Compute, {});
  std::vector<VkSurfaceKHR> surfaces;
  rhi.init(surfaces);
  testDifferential(rhi, false);
  testDifferential(rhi, true);
  testOneElementOverflows(rhi);
  std::cout << "GPU terrain Phase 3 raster differential tests passed\n";
  os::Logger::shutdown();
  return 0;
}
