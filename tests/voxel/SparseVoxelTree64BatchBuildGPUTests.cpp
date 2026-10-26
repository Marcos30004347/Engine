#include "os/Logger.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/SparseVoxelTree64.hpp"
#include "voxel/svraster/rendering/SparseVoxelTree64BatchBuildChain.hpp"
#include "voxel/svraster/rendering/SparseVoxelTree64TracePass.hpp"

#include <algorithm>
#include <array>
// These tests use assert() as their check mechanism, and Release builds
// define NDEBUG. Without this the assertions vanish - and because many of
// them wrap the call they are checking, the call under test vanishes with
// them, leaving objects unconfigured and the suite crashing or silently
// passing. cassert re-reads NDEBUG on every include, so undefining it here
// restores assert regardless of what included cassert earlier.
#undef NDEBUG
#include <cassert>
#include <cmath>
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

const SparseVoxelTree64NodeGPU kSentinel{
  0xA17EA17Eu,
  0xC0DEC0DEu,
  0x51A751A7u,
};

SparseVoxelTree64BuildSourceGPU gpuSource(const SparseVoxelTree64AABB &box, const uint32_t jobIndex)
{
  return {
    {static_cast<float>(box.minimum[0]), static_cast<float>(box.minimum[1]), static_cast<float>(box.minimum[2])},
    jobIndex,
    {static_cast<float>(box.maximum[0]), static_cast<float>(box.maximum[1]), static_cast<float>(box.maximum[2])},
    0u,
  };
}

SparseVoxelTree64AABB transformedFixture()
{
  // A 90-degree Z rotation followed by translation. Transforming all eight
  // corners gives this exact chunk-local conservative bound.
  const SparseVoxelTree64AABB model{
    {0.25, 1.25, 2.25},
    {2.75, 3.75, 4.75},
  };
  SparseVoxelTree64AABB result{
    {1.0e30, 1.0e30, 1.0e30},
    {-1.0e30, -1.0e30, -1.0e30},
  };
  for (uint32_t corner = 0u; corner < 8u; ++corner)
  {
    const double x = (corner & 1u) != 0u ? model.maximum[0] : model.minimum[0];
    const double y = (corner & 2u) != 0u ? model.maximum[1] : model.minimum[1];
    const double z = (corner & 4u) != 0u ? model.maximum[2] : model.minimum[2];
    const std::array<double, 3> transformed{
      -y + 8.0,
      x + 5.0,
      z + 1.0,
    };
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      result.minimum[axis] = std::min(result.minimum[axis], transformed[axis]);
      result.maximum[axis] = std::max(result.maximum[axis], transformed[axis]);
    }
  }
  return result;
}

std::vector<std::vector<SparseVoxelTree64AABB>> makeFixtures(const uint32_t side)
{
  std::vector<std::vector<SparseVoxelTree64AABB>> fixtures(3u);
  fixtures[1].push_back({
    {0.0, 0.0, 0.0},
    {static_cast<double>(side), static_cast<double>(side), static_cast<double>(side)},
  });
  fixtures[2] = {
    {{1.125, 2.125, 3.125}, {1.875, 2.875, 3.875}},
    {{2.25, 2.25, 2.25}, {std::min<double>(side, 8.75), std::min<double>(side, 9.75), std::min<double>(side, 7.75)}},
    {{3.5, 3.5, 3.5}, {std::min<double>(side, 10.5), std::min<double>(side, 10.5), std::min<double>(side, 10.5)}},
  };
  if (side >= 16u)
    fixtures[2].push_back(transformedFixture());

  std::mt19937 random(0xBA7C000u + side);
  const uint32_t randomSourceCount = side == 64u ? 192u : 32u;
  for (uint32_t source = 0u; source < randomSourceCount; ++source)
  {
    const uint32_t x = random() % side;
    const uint32_t y = random() % side;
    const uint32_t z = random() % side;
    fixtures[2].push_back({
      {static_cast<double>(x) + 0.125, static_cast<double>(y) + 0.125, static_cast<double>(z) + 0.125},
      {static_cast<double>(x) + 0.875, static_cast<double>(y) + 0.875, static_cast<double>(z) + 0.875},
    });
  }
  return fixtures;
}

void testBatchSide(::rendering::backend::vulkan::VulkanRHI &rhi, const uint32_t side)
{
  const auto fixtures = makeFixtures(side);
  const uint32_t chunkCount = static_cast<uint32_t>(fixtures.size());
  const uint32_t slotCapacity = sparseVoxelTree64MaximumNodeCount(side);
  const uint32_t nodeCount = chunkCount * slotCapacity + 7u;
  std::vector<SparseVoxelTree64Chunk> expected(chunkCount);
  std::vector<SparseVoxelTree64BuildJobGPU> jobs(chunkCount);
  std::vector<SparseVoxelTree64BuildSourceGPU> sources;
  uint32_t generation = 81u;
  for (uint32_t jobIndex = 0u; jobIndex < chunkCount; ++jobIndex)
  {
    std::string error;
    assert(buildSparseVoxelTree64(side, fixtures[jobIndex], expected[jobIndex], &error));
    jobs[jobIndex].worldChunk = {static_cast<int32_t>(jobIndex), 0, 0};
    jobs[jobIndex].nodeSlot = jobIndex;
    jobs[jobIndex].contentGeneration = generation;
    jobs[jobIndex].flags = kSparseVoxelTree64BuildJobValid;
    for (const auto &box : fixtures[jobIndex])
      sources.push_back(gpuSource(box, jobIndex));
  }

  RenderGraph graph(&rhi);
  const Buffer nodePool = graph.createBuffer({
    .name = "Tree64Batch.Nodes",
    .size = uint64_t{nodeCount} * sizeof(SparseVoxelTree64NodeGPU),
    .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
  });
  std::vector<SparseVoxelTree64NodeGPU> initial(nodeCount, kSentinel);
  graph.bufferWrite(nodePool, 0u, uint64_t{initial.size()} * sizeof(SparseVoxelTree64NodeGPU), initial.data());

  const Buffer nodeReadback = graph.createBuffer({
    .name = "Tree64Batch.NodeReadback",
    .size = uint64_t{nodeCount} * sizeof(SparseVoxelTree64NodeGPU),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  const Buffer resultReadback = graph.createBuffer({
    .name = "Tree64Batch.ResultReadback",
    .size = uint64_t{chunkCount} * sizeof(SparseVoxelTree64BuildResultGPU),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  uint32_t passIndex = 0u;
  {
    svrRendering::SparseVoxelTree64BatchBuildChain chain(
        graph,
        passIndex,
        nodePool,
        {
          side,
          chunkCount,
          static_cast<uint32_t>(sources.size()),
          slotCapacity,
          nodeCount,
        },
        "Tree64Batch");
    assert(chain.update(jobs, sources));
    auto *copyNodes = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
        "Tree64Batch.CopyNodes", passIndex++, nodePool, 0u, uint64_t{nodeCount} * sizeof(SparseVoxelTree64NodeGPU), nodeReadback, 0u, uint64_t{nodeCount} * sizeof(SparseVoxelTree64NodeGPU));
    auto *copyResults = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
        "Tree64Batch.CopyResults", passIndex++, chain.results(), 0u, uint64_t{chunkCount} * sizeof(SparseVoxelTree64BuildResultGPU), resultReadback, 0u, uint64_t{chunkCount} * sizeof(SparseVoxelTree64BuildResultGPU));

    graph.compile();
    RenderGraph::Frame frame{};
    RenderGraph::Overrides overrides{};
    graph.run(frame, overrides);
    graph.waitFrame(frame);
    std::vector<SparseVoxelTree64NodeGPU> actualNodes(nodeCount);
    std::vector<SparseVoxelTree64BuildResultGPU> results(chunkCount);
    graph.bufferRead(
        nodeReadback,
        0u,
        uint64_t{actualNodes.size()} * sizeof(SparseVoxelTree64NodeGPU),
        [&](const void *data)
        {
          std::memcpy(actualNodes.data(), data, uint64_t{actualNodes.size()} * sizeof(SparseVoxelTree64NodeGPU));
        });
    graph.bufferRead(
        resultReadback,
        0u,
        uint64_t{results.size()} * sizeof(SparseVoxelTree64BuildResultGPU),
        [&](const void *data)
        {
          std::memcpy(results.data(), data, uint64_t{results.size()} * sizeof(SparseVoxelTree64BuildResultGPU));
        });
    for (uint32_t jobIndex = 0u; jobIndex < chunkCount; ++jobIndex)
    {
      assert(results[jobIndex].status == kSparseVoxelTree64BuildSuccess);
      if (results[jobIndex].nodeCount != expected[jobIndex].nodes.size())
        std::cerr << "Tree64 batch node-count mismatch side " << side << " job " << jobIndex << ": expected " << expected[jobIndex].nodes.size() << ", actual " << results[jobIndex].nodeCount << '\n';
      assert(results[jobIndex].nodeCount == expected[jobIndex].nodes.size());
      assert(results[jobIndex].nodeSlot == jobIndex);
      assert(results[jobIndex].contentGeneration == generation);
      const auto actualBegin = actualNodes.begin() + uint64_t{jobIndex} * slotCapacity;
      assert(
          std::equal(
              expected[jobIndex].nodes.begin(),
              expected[jobIndex].nodes.end(),
              actualBegin,
              [](const auto &left, const auto &right)
              {
                return std::memcmp(&left, &right, sizeof(SparseVoxelTree64NodeGPU)) == 0;
              }));
      assert(
          std::all_of(
              actualBegin + expected[jobIndex].nodes.size(),
              actualNodes.begin() + uint64_t{jobIndex + 1u} * slotCapacity,
              [](const auto &node)
              {
                return std::memcmp(&node, &kSentinel, sizeof(node)) == 0;
              }));
    }
    assert(
        std::all_of(
            actualNodes.begin() + uint64_t{chunkCount} * slotCapacity,
            actualNodes.end(),
            [](const auto &node)
            {
              return std::memcmp(&node, &kSentinel, sizeof(node)) == 0;
            }));

    delete copyResults;
    delete copyNodes;
  }
  graph.deleteBuffer(resultReadback);
  graph.deleteBuffer(nodeReadback);
  graph.deleteBuffer(nodePool);
}

void testBuildToTrace(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  constexpr uint32_t side = 16u;
  const uint32_t slotCapacity = sparseVoxelTree64MaximumNodeCount(side);
  const uint32_t nodeCount = 2u * slotCapacity;
  const std::vector<std::vector<SparseVoxelTree64AABB>> fixtures{
    {},
    {{{0.0, 0.0, 0.0}, {16.0, 16.0, 16.0}}},
  };
  std::vector<SparseVoxelTree64Chunk> expected(2u);
  std::vector<SparseVoxelTree64BuildJobGPU> jobs(2u);
  std::vector<SparseVoxelTree64BuildSourceGPU> sources;
  for (uint32_t index = 0u; index < 2u; ++index)
  {
    std::string error;
    assert(buildSparseVoxelTree64(side, fixtures[index], expected[index], &error));
    jobs[index].worldChunk = {static_cast<int32_t>(index), 0, 0};
    jobs[index].nodeSlot = index;
    jobs[index].contentGeneration = 91u;
    jobs[index].flags = kSparseVoxelTree64BuildJobValid;
    for (const auto &box : fixtures[index])
      sources.push_back(gpuSource(box, index));
  }

  SparseVoxelTree64StateGPU state{};
  state.logicalChunkResolution = {2u, 1u, 1u};
  state.chunkSide = side;
  state.valid = 1u;
  state.contentGeneration = 91u;
  state.logicalCellResolution = {32u, 16u, 16u};
  state.cellSize = {1.0f, 1.0f, 1.0f};
  state.worldExtent = {32.0f, 16.0f, 16.0f};
  std::vector<SparseVoxelTree64ChunkGPU> directory(2u);
  for (uint32_t index = 0u; index < 2u; ++index)
  {
    directory[index].worldChunk = {static_cast<int32_t>(index), 0, 0};
    directory[index].nodeSlot = index;
    directory[index].nodeCount = static_cast<uint32_t>(expected[index].nodes.size());
    directory[index].contentGeneration = 91u;
    directory[index].flags = kSparseVoxelTree64ChunkValid;
    if (sparseVoxelTree64PopulationMask(expected[index].nodes.front()) != 0u)
      directory[index].flags |= kSparseVoxelTree64ChunkOccupied;
  }
  std::vector<SparseVoxelOccupancyDDARayGPU> rays{
    {{-10.0f, 0.5f, 0.5f, 80.0f}, {1.0f, 0.0f, 0.0f, 0.0f}},
    {{42.0f, 0.5f, 0.5f, 80.0f}, {-1.0f, 0.0f, 0.0f, 0.0f}},
    {{-10.0f, 0.5f, 0.5f, 5.0f}, {1.0f, 0.0f, 0.0f, 0.0f}},
  };

  RenderGraph graph(&rhi);
  const auto uploadBuffer = [&](const std::string &name, const uint64_t size, const BufferUsage usage)
  {
    return graph.createBuffer({
      .name = name,
      .size = size,
      .usage = usage | BufferUsage::BufferUsage_Push,
      .frameLocal = true,
    });
  };
  const Buffer nodePool = graph.createBuffer({
    .name = "Tree64BuildTrace.Nodes",
    .size = uint64_t{nodeCount} * sizeof(SparseVoxelTree64NodeGPU),
    .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
  });
  const Buffer stateBuffer = uploadBuffer("Tree64BuildTrace.State", sizeof(state), BufferUsage::BufferUsage_Storage);
  const Buffer chunkBuffer = uploadBuffer("Tree64BuildTrace.Chunks", directory.size() * sizeof(SparseVoxelTree64ChunkGPU), BufferUsage::BufferUsage_Storage);
  const Buffer rayBuffer = uploadBuffer("Tree64BuildTrace.Rays", rays.size() * sizeof(SparseVoxelOccupancyDDARayGPU), BufferUsage::BufferUsage_Storage);
  const Buffer resultBuffer = graph.createBuffer({
    .name = "Tree64BuildTrace.Results",
    .size = rays.size() * sizeof(SparseVoxelOccupancyDDAResultGPU),
    .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
  });
  const Buffer readback = graph.createBuffer({
    .name = "Tree64BuildTrace.Readback",
    .size = rays.size() * sizeof(SparseVoxelOccupancyDDAResultGPU),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  graph.bufferWrite(stateBuffer, 0u, sizeof(state), &state);
  graph.bufferWrite(chunkBuffer, 0u, directory.size() * sizeof(SparseVoxelTree64ChunkGPU), directory.data());
  graph.bufferWrite(rayBuffer, 0u, rays.size() * sizeof(SparseVoxelOccupancyDDARayGPU), rays.data());

  uint32_t passIndex = 0u;
  {
    svrRendering::SparseVoxelTree64BatchBuildChain chain(
        graph,
        passIndex,
        nodePool,
        {
          side,
          2u,
          static_cast<uint32_t>(sources.size()),
          slotCapacity,
          nodeCount,
        },
        "Tree64BuildTrace.Build");
    assert(chain.update(jobs, sources));
    auto *trace = graph.registerPass<svrRendering::SparseVoxelTree64TracePass>(
        "Tree64BuildTrace.Trace",
        passIndex++,
        svrRendering::SparseVoxelTree64TracePass::Buffers{
          stateBuffer,
          chunkBuffer,
          nodePool,
          rayBuffer,
          resultBuffer,
        },
        svrRendering::SparseVoxelTree64TracePass::Capacities{
          2u,
          nodeCount,
          slotCapacity,
          static_cast<uint32_t>(rays.size()),
        });
    assert(trace->update(static_cast<uint32_t>(rays.size()), 4096u));
    auto *copy = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
        "Tree64BuildTrace.Copy", passIndex++, resultBuffer, 0u, rays.size() * sizeof(SparseVoxelOccupancyDDAResultGPU), readback, 0u, rays.size() * sizeof(SparseVoxelOccupancyDDAResultGPU));
    graph.compile();
    RenderGraph::Frame frame{};
    RenderGraph::Overrides overrides{};
    graph.run(frame, overrides);
    graph.waitFrame(frame);
    std::vector<SparseVoxelOccupancyDDAResultGPU> actual(rays.size());
    graph.bufferRead(
        readback,
        0u,
        actual.size() * sizeof(SparseVoxelOccupancyDDAResultGPU),
        [&](const void *data)
        {
          std::memcpy(actual.data(), data, actual.size() * sizeof(SparseVoxelOccupancyDDAResultGPU));
        });
    assert(actual[0].status == 2u);
    assert((actual[0].globalCell == std::array<int32_t, 3>{16, 0, 0}));
    assert(std::abs(actual[0].distance - 26.0f) < 1.0e-4f);
    assert(actual[1].status == 2u);
    assert((actual[1].globalCell == std::array<int32_t, 3>{31, 0, 0}));
    assert(std::abs(actual[1].distance - 10.0f) < 1.0e-4f);
    assert(actual[2].status == 1u);
    delete copy;
    delete trace;
  }
  graph.deleteBuffer(readback);
  graph.deleteBuffer(resultBuffer);
  graph.deleteBuffer(rayBuffer);
  graph.deleteBuffer(chunkBuffer);
  graph.deleteBuffer(stateBuffer);
  graph.deleteBuffer(nodePool);
}

void testBatchOverflow(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  constexpr uint32_t side = 16u;
  constexpr uint32_t nodeCount = 11u;
  constexpr uint32_t slotCapacity = 4u;
  const SparseVoxelTree64BuildJobGPU job{
    {0, 0, 0},
    0u,
    101u,
    kSparseVoxelTree64BuildJobValid,
    0u,
    0u,
  };
  const SparseVoxelTree64BuildSourceGPU source = gpuSource({{0.0, 0.0, 0.0}, {16.0, 16.0, 16.0}}, 0u);

  RenderGraph graph(&rhi);
  const Buffer nodePool = graph.createBuffer({
    .name = "Tree64BatchOverflow.Nodes",
    .size = nodeCount * sizeof(SparseVoxelTree64NodeGPU),
    .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
  });
  const Buffer nodeReadback = graph.createBuffer({
    .name = "Tree64BatchOverflow.NodeReadback",
    .size = nodeCount * sizeof(SparseVoxelTree64NodeGPU),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  const Buffer resultReadback = graph.createBuffer({
    .name = "Tree64BatchOverflow.ResultReadback",
    .size = sizeof(SparseVoxelTree64BuildResultGPU),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  std::vector<SparseVoxelTree64NodeGPU> initial(nodeCount, kSentinel);
  graph.bufferWrite(nodePool, 0u, initial.size() * sizeof(SparseVoxelTree64NodeGPU), initial.data());
  uint32_t passIndex = 0u;
  {
    svrRendering::SparseVoxelTree64BatchBuildChain chain(
        graph,
        passIndex,
        nodePool,
        {
          side,
          1u,
          1u,
          slotCapacity,
          nodeCount,
        },
        "Tree64BatchOverflow");
    assert(chain.update({job}, {source}));
    auto *copyNodes = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
        "Tree64BatchOverflow.CopyNodes", passIndex++, nodePool, 0u, nodeCount * sizeof(SparseVoxelTree64NodeGPU), nodeReadback, 0u, nodeCount * sizeof(SparseVoxelTree64NodeGPU));
    auto *copyResult = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
        "Tree64BatchOverflow.CopyResult", passIndex++, chain.results(), 0u, sizeof(SparseVoxelTree64BuildResultGPU), resultReadback, 0u, sizeof(SparseVoxelTree64BuildResultGPU));
    graph.compile();
    RenderGraph::Frame frame{};
    RenderGraph::Overrides overrides{};
    graph.run(frame, overrides);
    graph.waitFrame(frame);
    std::vector<SparseVoxelTree64NodeGPU> actual(nodeCount);
    SparseVoxelTree64BuildResultGPU result{};
    graph.bufferRead(
        nodeReadback,
        0u,
        actual.size() * sizeof(SparseVoxelTree64NodeGPU),
        [&](const void *data)
        {
          std::memcpy(actual.data(), data, actual.size() * sizeof(SparseVoxelTree64NodeGPU));
        });
    graph.bufferRead(
        resultReadback,
        0u,
        sizeof(result),
        [&](const void *data)
        {
          std::memcpy(&result, data, sizeof(result));
        });
    assert(result.status == kSparseVoxelTree64BuildOverflow);
    assert(result.overflowStage == kSparseVoxelTree64BuildOverflowPreflight);
    assert(
        std::all_of(
            actual.begin(),
            actual.end(),
            [](const auto &node)
            {
              return std::memcmp(&node, &kSentinel, sizeof(node)) == 0;
            }));
    delete copyResult;
    delete copyNodes;
  }
  graph.deleteBuffer(resultReadback);
  graph.deleteBuffer(nodeReadback);
  graph.deleteBuffer(nodePool);
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
  testBatchSide(rhi, 4u);
  testBatchSide(rhi, 16u);
  testBatchSide(rhi, 64u);
  testBuildToTrace(rhi);
  testBatchOverflow(rhi);
  std::cout << "Sparse voxel Tree64 T3 batched GPU build tests passed\n";
  os::Logger::shutdown();
  return 0;
}
