#include "os/Logger.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "voxel/svraster/SparseVoxelTree64Reference.hpp"
#include "voxel/svraster/rendering/TerrainGPUTree64BuildChain.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#undef NDEBUG
#include <cassert>

namespace
{
using namespace ::rendering;
namespace svr = voxel::svraster;
namespace svrRendering = voxel::svraster::rendering;

svr::WorldMapTerrainColumnPage testPage(const uint32_t height = 64u)
{
  svr::WorldMapTerrainColumnPage page{};
  page.side = 8u;
  page.maximumVerticalVoxels = height;
  page.internalLOD = 2u;
  page.heightUnitMetres = 0.5 /
      svr::kWorldMapTerrainHeightUnitsPerVoxel;
  page.voxelSizeMetres = 0.5;
  page.edgeMetres = 4.0;
  page.columns.resize(64u);
  page.walls.resize(64u);
  page.canonicalCornerHeights.resize(81u);
  page.canonicalBoundaryMinimumHeights.resize(32u);
  page.canonicalBoundaryMaximumHeights.resize(32u);
  for (uint32_t z = 0u; z < 8u; ++z)
    for (uint32_t x = 0u; x < 8u; ++x)
    {
      const uint32_t index = z * 8u + x;
      const uint32_t top = x == 0u && z == 0u
          ? height - 1u : std::min(height - 1u, 4u + x * 3u + z * 2u);
      const uint32_t first = top > 6u ? top - (3u + (x + z) % 4u) : 0u;
      page.columns[index] = {
        top, first,
        static_cast<uint32_t>(svr::SparseVoxelMicroContext::FlatSurface)
            << 8u,
        0u,
      };
      page.walls[index] = {
        first, top == 0u ? 0u : top - 1u,
        static_cast<uint32_t>(svr::SparseVoxelMicroContext::WallNegativeX) |
            (static_cast<uint32_t>(
                svr::SparseVoxelMicroContext::WallNegativeZ) << 8u),
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
  return static_cast<uint32_t>(total);
}

svr::SparseVoxelTerrainPageTree64 reference(
    const svr::WorldMapTerrainColumnPage &page)
{
  svr::SparseVoxelTerrainPageTree64 result{};
  std::string error;
  assert(svr::buildSparseVoxelTerrainPageTree64FromColumns(
      page, result, &error));
  return result;
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

svr::TerrainGPUTree64ResultGPU runBuild(
    ::rendering::backend::vulkan::VulkanRHI &rhi,
    const svr::WorldMapTerrainColumnPage &page,
    const svr::TerrainGPUTree64Capacity capacity,
    const bool splitAcrossFrames,
    std::vector<svr::SparseVoxelTree64NodeGPU> *readNodes)
{
  svr::TerrainGPUTree64ConfigGPU config{};
  std::string error;
  assert(svr::planTerrainGPUTree64Build(page, capacity, 23u,
      0xfedcba9876543210ull, 17u, 29u, 31u, config, &error));
  RenderGraph graph(&rhi);
  const auto pull = BufferUsage::BufferUsage_Pull |
      BufferUsage::BufferUsage_CopyDst;
  const Buffer nodeReadback = graph.createBuffer({
    .name = "Phase4.Nodes.Readback",
    .size = uint64_t{capacity.nodes} * sizeof(svr::SparseVoxelTree64NodeGPU),
    .usage = pull,
  });
  const Buffer resultReadback = graph.createBuffer({
    .name = "Phase4.Result.Readback",
    .size = sizeof(svr::TerrainGPUTree64ResultGPU), .usage = pull,
  });
  uint32_t passIndex = 0u;
  svr::TerrainGPUTree64ResultGPU result{};
  {
    svrRendering::TerrainGPUTree64BuildChain chain(
        graph, passIndex, capacity, static_cast<uint32_t>(page.columns.size()));
    auto *copyNodes = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
        "Phase4.Copy.Nodes", passIndex++, chain.nodes(), 0u,
        uint64_t{capacity.nodes} * sizeof(svr::SparseVoxelTree64NodeGPU),
        nodeReadback, 0u,
        uint64_t{capacity.nodes} * sizeof(svr::SparseVoxelTree64NodeGPU));
    auto *copyResult = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
        "Phase4.Copy.Result", passIndex++, chain.result(), 0u,
        sizeof(result), resultReadback, 0u, sizeof(result));
    graph.compile();
    RenderGraph::Frame frame{};
    RenderGraph::Overrides overrides{};
    if (!splitAcrossFrames)
    {
      assert(chain.update(page, config));
      graph.run(frame, overrides);
      graph.waitFrame(frame);
    }
    else
    {
      for (uint32_t stage = 0u;
           stage < svrRendering::TerrainGPUTree64BuildChain::kPassCount;
           ++stage)
      {
        assert(chain.update(page, config,
            static_cast<uint16_t>(1u << stage)));
        graph.run(frame, overrides);
        graph.waitFrame(frame);
      }
    }
    result = readVector<svr::TerrainGPUTree64ResultGPU>(
        graph, resultReadback, 1u).front();
    if (readNodes != nullptr && result.nodeCount <= capacity.nodes)
      *readNodes = readVector<svr::SparseVoxelTree64NodeGPU>(
          graph, nodeReadback, result.nodeCount);
    delete copyResult;
    delete copyNodes;
  }
  graph.deleteBuffer(resultReadback);
  graph.deleteBuffer(nodeReadback);
  return result;
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
      expected = cell[1] >= column.firstSolidY &&
          cell[1] <= column.topY;
    }
    assert(svr::sparseVoxelTree64Occupied(tree, cell) == expected);
  }
}

void assertRayParity(const svr::WorldMapTerrainColumnPage &page,
    const svr::SparseVoxelTree64Chunk &tree)
{
  const size_t cellCount = size_t{tree.side} * tree.side * tree.side;
  std::vector<uint8_t> cells(cellCount, 0u);
  for (uint32_t z = 0u; z < page.side; ++z)
    for (uint32_t x = 0u; x < page.side; ++x)
    {
      const auto &column = page.columns[z * page.side + x];
      for (uint32_t y = column.firstSolidY; y <= column.topY; ++y)
        cells[x + size_t{tree.side} *
            (y + size_t{tree.side} * z)] = 1u;
    }

  std::mt19937 random(0x7f4a7c15u);
  uint32_t hits = 0u;
  for (uint32_t sample = 0u; sample < 512u; ++sample)
  {
    std::array<double, 3> target{
      0.5 + double(random() % tree.side),
      0.5 + double(random() % tree.side),
      0.5 + double(random() % tree.side),
    };
    std::array<double, 3> origin = target;
    const uint32_t axis = (sample / 2u) % 3u;
    origin[axis] = (sample & 1u) == 0u
        ? -1.0 : double(tree.side) + 1.0;
    std::array<double, 3> direction{};
    double lengthSquared = 0.0;
    for (uint32_t component = 0u; component < 3u; ++component)
    {
      direction[component] = target[component] - origin[component];
      lengthSquared += direction[component] * direction[component];
    }
    const double length = std::sqrt(lengthSquared);
    for (double &component : direction) component /= length;
    const svr::SparseVoxelTree64Ray ray{
      origin, direction, length + double(tree.side) * 2.0};
    const auto expected = svr::traceSparseVoxelTree64CellsDDA(
        tree.side, cells, ray);
    const auto actual = svr::traceSparseVoxelTree64(tree, ray);
    assert(actual.status == expected.status);
    if (!actual.hit()) continue;
    ++hits;
    assert(actual.cell == expected.cell);
    assert(std::abs(actual.distance - expected.distance) < 1.0e-9);
  }
  assert(hits != 0u);
}

std::vector<svr::SparseVoxelTree64NodeGPU> testDifferential(
    ::rendering::backend::vulkan::VulkanRHI &rhi,
    const uint32_t height, const bool splitAcrossFrames)
{
  const auto page = testPage(height);
  const auto expected = reference(page);
  const svr::TerrainGPUTree64Capacity capacity{
    maskCapacity(page), static_cast<uint32_t>(expected.tree.nodes.size())};
  std::vector<svr::SparseVoxelTree64NodeGPU> nodes;
  const auto result = runBuild(
      rhi, page, capacity, splitAcrossFrames, &nodes);
  assert(result.status ==
      static_cast<uint32_t>(svr::TerrainGPUTree64Status::Ready));
  assert(result.validationErrors == 0u);
  assert(result.nodeCount == expected.tree.nodes.size());
  assert(result.visitedNodeCount == result.nodeCount);
  assert(nodes.size() == expected.tree.nodes.size());
  svr::SparseVoxelTree64Chunk actual{expected.side, nodes};
  std::string error;
  assert(svr::validateTerrainGPUTree64Chunk(actual, &error));
  assertColumnOccupancy(page, actual);
  assertRayParity(page, actual);
  const uint64_t hash = svr::terrainGPUTree64SemanticHash(actual);
  assert(result.semanticHashLow == static_cast<uint32_t>(hash));
  assert(result.semanticHashHigh == static_cast<uint32_t>(hash >> 32u));
  assert(result.descriptor.flags == svr::kSparseVoxelTree64TerrainPageValid);
  assert(result.descriptor.terrainSlot == 17u);
  assert(result.descriptor.terrainGeneration == 23u);
  assert(result.descriptor.internalLOD == page.internalLOD);
  assert(result.descriptor.nodePageSlot == 29u);
  assert(result.descriptor.nodePageGeneration == 31u);
  assert(result.descriptor.nodeCount == result.nodeCount);
  assert(result.descriptor.modelSide == expected.side);
  return nodes;
}

void testOverflow(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  const auto page = testPage();
  const auto expected = reference(page);
  const svr::TerrainGPUTree64Capacity capacity{
    maskCapacity(page),
    static_cast<uint32_t>(expected.tree.nodes.size()) - 1u,
  };
  const auto result = runBuild(rhi, page, capacity, false, nullptr);
  assert(result.status ==
      static_cast<uint32_t>(svr::TerrainGPUTree64Status::Overflow));
  assert(result.overflowStage ==
      static_cast<uint32_t>(svr::TerrainGPUTree64OverflowStage::Nodes));
}
} // namespace

int main()
{
  os::Logger::start();
  ::rendering::DeviceRequiredLimits limits{
    .minimumMemory = 0u,
    .minimumComputeSharedMemory = 0u,
    .minimumComputeWorkGroupInvocations = 64u,
  };
  ::rendering::backend::vulkan::VulkanRHI rhi(
      ::rendering::backend::vulkan::Vulkan_1_2, limits,
      ::rendering::DeviceFeatures::DeviceFeatures_Compute, {});
  std::vector<VkSurfaceKHR> surfaces;
  rhi.init(surfaces);
  const auto full = testDifferential(rhi, 64u, false);
  const auto sliced = testDifferential(rhi, 64u, true);
  assert(full.size() == sliced.size());
  assert(std::memcmp(full.data(), sliced.data(),
      full.size() * sizeof(full.front())) == 0);
  testDifferential(rhi, 128u, false);
  testOverflow(rhi);
  std::cout << "GPU terrain Phase 4 Tree64 differential tests passed\n";
  os::Logger::shutdown();
  return 0;
}
