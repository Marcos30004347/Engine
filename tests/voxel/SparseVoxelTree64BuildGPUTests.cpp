#include "os/Logger.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "voxel/svraster/SparseVoxelTree64.hpp"
#include "voxel/svraster/rendering/SparseVoxelTree64BuildPass.hpp"

#include <algorithm>
// These tests use assert() as their check mechanism, and Release builds
// define NDEBUG. Without this the assertions vanish - and because many of
// them wrap the call they are checking, the call under test vanishes with
// them, leaving objects unconfigured and the suite crashing or silently
// passing. cassert re-reads NDEBUG on every include, so undefining it here
// restores assert regardless of what included cassert earlier.
#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace
{

using namespace ::rendering;
using namespace voxel::svraster;
namespace svrRendering = voxel::svraster::rendering;

constexpr uint32_t kSide = 16u;

size_t cellIndex(const uint32_t x, const uint32_t y, const uint32_t z)
{
  return static_cast<size_t>(x) + static_cast<size_t>(kSide) * (static_cast<size_t>(y) + static_cast<size_t>(kSide) * z);
}

void testSingleChunkBuild(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  const uint32_t cellCount = kSide * kSide * kSide;
  const uint32_t slotCapacity = sparseVoxelTree64MaximumNodeCount(kSide);
  const uint32_t slotCount = 2u;
  const uint32_t nodeCount = slotCount * slotCapacity;
  const SparseVoxelTree64NodeGPU sentinel{
    0xA5A5A5A5u,
    0xC3C3C3C3u,
    0x5A5A5A5Au,
  };

  RenderGraph graph(&rhi);
  const Buffer cellsBuffer = graph.createBuffer({
    .name = "Tree64Build.Cells",
    .size = uint64_t{cellCount} * sizeof(uint32_t),
    .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
    .frameLocal = true,
  });
  const Buffer nodesBuffer = graph.createBuffer({
    .name = "Tree64Build.Nodes",
    .size = uint64_t{nodeCount} * sizeof(SparseVoxelTree64NodeGPU),
    .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
  });
  const Buffer resultBuffer = graph.createBuffer({
    .name = "Tree64Build.Result",
    .size = sizeof(SparseVoxelTree64BuildResultGPU),
    .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
  });
  const Buffer nodesReadback = graph.createBuffer({
    .name = "Tree64Build.NodesReadback",
    .size = uint64_t{nodeCount} * sizeof(SparseVoxelTree64NodeGPU),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  const Buffer resultReadback = graph.createBuffer({
    .name = "Tree64Build.ResultReadback",
    .size = sizeof(SparseVoxelTree64BuildResultGPU),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });

  uint32_t passIndex = 0u;
  auto *build = graph.registerPass<svrRendering::SparseVoxelTree64BuildPass>(
      "Tree64Build.Build",
      passIndex++,
      svrRendering::SparseVoxelTree64BuildPass::Buffers{
        cellsBuffer,
        nodesBuffer,
        resultBuffer,
      },
      svrRendering::SparseVoxelTree64BuildPass::Capacities{
        cellCount,
        nodeCount,
      });
  auto *copyNodes = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "Tree64Build.CopyNodes", passIndex++, nodesBuffer, 0u, uint64_t{nodeCount} * sizeof(SparseVoxelTree64NodeGPU), nodesReadback, 0u, uint64_t{nodeCount} * sizeof(SparseVoxelTree64NodeGPU));
  auto *copyResult =
      graph.registerPass<::rendering::gpgpu::CopyBufferPass>("Tree64Build.CopyResult", passIndex++, resultBuffer, 0u, sizeof(SparseVoxelTree64BuildResultGPU), resultReadback, 0u, sizeof(SparseVoxelTree64BuildResultGPU));
  graph.compile();

  std::vector<std::vector<uint8_t>> fixtures;
  fixtures.emplace_back(cellCount, 0u);
  fixtures.emplace_back(cellCount, 0u);
  fixtures.back()[cellIndex(15u, 0u, 7u)] = 1u;
  fixtures.emplace_back(cellCount, 0u);
  for (uint32_t z = 0u; z < kSide; ++z)
    for (uint32_t y = 0u; y < kSide; ++y)
      for (uint32_t x = 0u; x < kSide; ++x)
        if (((x * 17u + y * 11u + z * 5u) % 29u) == 0u)
          fixtures.back()[cellIndex(x, y, z)] = 1u;
  fixtures.emplace_back(cellCount, 1u);
  std::mt19937 random(0xB01764u);
  for (uint32_t fixture = 0u; fixture < 8u; ++fixture)
  {
    fixtures.emplace_back(cellCount, 0u);
    for (uint8_t &cell : fixtures.back())
      cell = (random() % 13u) == 0u ? 1u : 0u;
  }

  const auto run = [&](const std::vector<uint8_t> &cells, const uint32_t destinationSlot, const uint32_t effectiveSlotCapacity, const uint32_t generation)
  {
    std::vector<uint32_t> gpuCells(cells.begin(), cells.end());
    std::vector<SparseVoxelTree64NodeGPU> initial(nodeCount, sentinel);
    SparseVoxelTree64BuildResultGPU initialResult{};
    graph.bufferWrite(cellsBuffer, 0u, uint64_t{gpuCells.size()} * sizeof(uint32_t), gpuCells.data());
    graph.bufferWrite(nodesBuffer, 0u, uint64_t{initial.size()} * sizeof(SparseVoxelTree64NodeGPU), initial.data());
    graph.bufferWrite(resultBuffer, 0u, sizeof(initialResult), &initialResult);
    assert(build->update(kSide, cellCount, destinationSlot, effectiveSlotCapacity, generation));

    RenderGraph::Frame frame{};
    RenderGraph::Overrides overrides{};
    graph.run(frame, overrides);
    graph.waitFrame(frame);

    SparseVoxelTree64BuildResultGPU result{};
    std::vector<SparseVoxelTree64NodeGPU> actual(nodeCount);
    graph.bufferRead(
        resultReadback,
        0u,
        sizeof(result),
        [&](const void *data)
        {
          std::memcpy(&result, data, sizeof(result));
        });
    graph.bufferRead(
        nodesReadback,
        0u,
        uint64_t{actual.size()} * sizeof(SparseVoxelTree64NodeGPU),
        [&](const void *data)
        {
          std::memcpy(actual.data(), data, uint64_t{actual.size()} * sizeof(SparseVoxelTree64NodeGPU));
        });
    return std::pair{result, actual};
  };

  uint32_t generation = 31u;
  uint32_t fixtureIndex = 0u;
  for (const auto &cells : fixtures)
  {
    SparseVoxelTree64Chunk expected;
    std::string error;
    assert(buildSparseVoxelTree64(kSide, cells, expected, &error));
    const auto [result, actual] = run(cells, 1u, slotCapacity, generation);
    assert(result.status == kSparseVoxelTree64BuildSuccess);
    assert(result.nodeCount == expected.nodes.size());
    assert(result.rootNodeOffset == 0u);
    assert(result.nodeSlot == 1u);
    assert(result.contentGeneration == generation);
    assert(result.overflowStage == 0u);
    assert(result.maximumStack <= 256u);
    const auto begin = actual.begin() + slotCapacity;
    const bool bytesMatch = std::equal(
        expected.nodes.begin(),
        expected.nodes.end(),
        begin,
        [](const auto &left, const auto &right)
        {
          return std::memcmp(&left, &right, sizeof(SparseVoxelTree64NodeGPU)) == 0;
        });
    if (!bytesMatch)
    {
      std::cerr << "Tree64 build mismatch fixture " << fixtureIndex << ", expected " << expected.nodes.size() << " nodes, actual " << result.nodeCount << '\n';
      for (uint32_t nodeIndex = 0u; nodeIndex < std::min<uint32_t>(12u, expected.nodes.size()); ++nodeIndex)
      {
        const auto &left = expected.nodes[nodeIndex];
        const auto &right = begin[nodeIndex];
        std::cerr << " node " << nodeIndex << " expected (" << left.childBaseAndFlags << ", " << left.populationMaskLo << ", " << left.populationMaskHi << "), actual (" << right.childBaseAndFlags << ", "
                  << right.populationMaskLo << ", " << right.populationMaskHi << ")\n";
      }
      for (uint32_t nodeIndex = 0u; nodeIndex < expected.nodes.size(); ++nodeIndex)
      {
        const auto &left = expected.nodes[nodeIndex];
        const auto &right = begin[nodeIndex];
        if (std::memcmp(&left, &right, sizeof(left)) != 0)
        {
          std::cerr << " first mismatch node " << nodeIndex << ": expected (" << left.childBaseAndFlags << ", " << left.populationMaskLo << ", " << left.populationMaskHi << "), actual (" << right.childBaseAndFlags
                    << ", " << right.populationMaskLo << ", " << right.populationMaskHi << ")\n";
          break;
        }
      }
    }
    assert(bytesMatch);
    assert(
        std::all_of(
            actual.begin(),
            actual.begin() + slotCapacity,
            [&](const auto &node)
            {
              return std::memcmp(&node, &sentinel, sizeof(node)) == 0;
            }));
    assert(
        std::all_of(
            begin + expected.nodes.size(),
            actual.end(),
            [&](const auto &node)
            {
              return std::memcmp(&node, &sentinel, sizeof(node)) == 0;
            }));
    ++generation;
    ++fixtureIndex;
  }

  const std::vector<uint8_t> dense(cellCount, 1u);
  const auto [overflow, unchanged] = run(dense, 0u, 4u, generation);
  assert(overflow.status == kSparseVoxelTree64BuildOverflow);
  assert(overflow.overflowStage == kSparseVoxelTree64BuildOverflowPreflight);
  assert(
      std::all_of(
          unchanged.begin(),
          unchanged.end(),
          [&](const auto &node)
          {
            return std::memcmp(&node, &sentinel, sizeof(node)) == 0;
          }));

  delete copyResult;
  delete copyNodes;
  delete build;
  graph.deleteBuffer(resultReadback);
  graph.deleteBuffer(nodesReadback);
  graph.deleteBuffer(resultBuffer);
  graph.deleteBuffer(nodesBuffer);
  graph.deleteBuffer(cellsBuffer);
}

} // namespace

int main()
{
  os::Logger::start();
  ::rendering::DeviceRequiredLimits limits{
    .minimumMemory = 0u,
    .minimumComputeSharedMemory = 0u,
    .minimumComputeWorkGroupInvocations = 256u,
  };
  ::rendering::backend::vulkan::VulkanRHI rhi(::rendering::backend::vulkan::Vulkan_1_2, limits, ::rendering::DeviceFeatures::DeviceFeatures_Compute, {});
  std::vector<VkSurfaceKHR> surfaces;
  rhi.init(surfaces);
  testSingleChunkBuild(rhi);
  std::cout << "Sparse voxel Tree64 T3 single-chunk GPU build tests passed\n";
  os::Logger::shutdown();
  return 0;
}
