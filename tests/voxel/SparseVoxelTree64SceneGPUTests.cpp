#include "os/Logger.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/SparseVoxelTree64.hpp"
#include "voxel/svraster/rendering/SparseVoxelTree64SceneBuildChain.hpp"

#include <array>
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
#include <vector>

namespace
{

using namespace ::rendering;
using namespace voxel::svraster;
namespace svrRendering = voxel::svraster::rendering;

std::array<float, 16> identityMatrix()
{
  return {
    1.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    1.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    1.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    1.0f,
  };
}

void testResidentSceneBuild(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  constexpr uint32_t side = 16u;
  constexpr uint32_t chunkCount = 2u;
  const uint32_t slotCapacity = sparseVoxelTree64MaximumNodeCount(side);
  const uint32_t nodeCount = chunkCount * slotCapacity;

  SparseVoxelInstanceGPU instance{};
  instance.modelToWorld = identityMatrix();
  instance.worldToModel = identityMatrix();
  instance.previousModelToWorld = identityMatrix();
  instance.worldBoundsMin = {2.0f, 1.0f, 1.0f, 0.0f};
  instance.worldBoundsMax = {17.0f, 3.0f, 3.0f, 0.0f};
  instance.blasIndex = 0u;
  const std::array<SparseVoxelLODNodeGPU, 3u> hierarchy{{
    {1u, 2u, 0u, 0u},
    {0u, 0u, 1u, 1u},
    {0u, 0u, 2u, 1u},
  }};
  const std::array<SparseVoxelCenterLengthGPU, 3u> centers{{
    {{9.5f, 2.0f, 2.0f}, 19.0f},
    {{2.5f, 2.0f, 2.0f}, 1.0f},
    {{16.5f, 2.0f, 2.0f}, 1.0f},
  }};
  const SparseVoxelBLASRootRangeGPU roots{0u, 1u, 1u, 0u};

  std::vector<SparseVoxelTree64BuildJobGPU> jobs(chunkCount);
  for (uint32_t index = 0u; index < chunkCount; ++index)
    jobs[index] = {
      {static_cast<int32_t>(index), 0, 0},
      index,
      41u,
      kSparseVoxelTree64BuildJobValid,
      0u,
      0u,
    };
  std::array<SparseVoxelTree64Chunk, chunkCount> expected{};
  std::string error;
  assert(buildSparseVoxelTree64(side, {{{2.0, 1.0, 1.0}, {3.0, 3.0, 3.0}}}, expected[0], &error));
  assert(buildSparseVoxelTree64(side, {{{0.0, 1.0, 1.0}, {1.0, 3.0, 3.0}}}, expected[1], &error));

  RenderGraph graph(&rhi);
  const auto upload = [&](const std::string &name, const uint64_t size)
  {
    return graph.createBuffer({
      .name = name,
      .size = size,
      .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
    });
  };
  const Buffer instances = upload("Tree64Scene.Instances", sizeof(instance));
  const Buffer hierarchyBuffer = upload("Tree64Scene.Hierarchy", sizeof(hierarchy));
  const Buffer centersBuffer = upload("Tree64Scene.Centers", sizeof(centers));
  const Buffer rootsBuffer = upload("Tree64Scene.Roots", sizeof(roots));
  const Buffer nodePool = graph.createBuffer({
    .name = "Tree64Scene.Nodes",
    .size = uint64_t{nodeCount} * sizeof(SparseVoxelTree64NodeGPU),
    .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
  });
  const Buffer nodeReadback = graph.createBuffer({
    .name = "Tree64Scene.NodeReadback",
    .size = uint64_t{nodeCount} * sizeof(SparseVoxelTree64NodeGPU),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  const Buffer resultReadback = graph.createBuffer({
    .name = "Tree64Scene.ResultReadback",
    .size = chunkCount * sizeof(SparseVoxelTree64BuildResultGPU),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  const Buffer counterReadback = graph.createBuffer({
    .name = "Tree64Scene.CounterReadback",
    .size = sizeof(SparseVoxelTree64SceneCountersGPU),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  const Buffer overflowNodePool = graph.createBuffer({
    .name = "Tree64Scene.OverflowNodes",
    .size = uint64_t{slotCapacity} * sizeof(SparseVoxelTree64NodeGPU),
    .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
  });
  const Buffer overflowNodeReadback = graph.createBuffer({
    .name = "Tree64Scene.OverflowNodeReadback",
    .size = uint64_t{slotCapacity} * sizeof(SparseVoxelTree64NodeGPU),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  const Buffer overflowResultReadback = graph.createBuffer({
    .name = "Tree64Scene.OverflowResultReadback",
    .size = sizeof(SparseVoxelTree64BuildResultGPU),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  const Buffer overflowCounterReadback = graph.createBuffer({
    .name = "Tree64Scene.OverflowCounterReadback",
    .size = sizeof(SparseVoxelTree64SceneCountersGPU),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  graph.bufferWrite(instances, 0u, sizeof(instance), &instance);
  graph.bufferWrite(hierarchyBuffer, 0u, sizeof(hierarchy), const_cast<SparseVoxelLODNodeGPU *>(hierarchy.data()));
  graph.bufferWrite(centersBuffer, 0u, sizeof(centers), const_cast<SparseVoxelCenterLengthGPU *>(centers.data()));
  graph.bufferWrite(rootsBuffer, 0u, sizeof(roots), const_cast<SparseVoxelBLASRootRangeGPU *>(&roots));

  uint32_t passIndex = 0u;
  {
    svrRendering::SparseVoxelTree64SceneBuildChain chain(
        graph,
        passIndex,
        nodePool,
        {
          side,
          chunkCount,
          slotCapacity,
          nodeCount,
        },
        {
          instances,
          hierarchyBuffer,
          centersBuffer,
          rootsBuffer,
          1u,
          static_cast<uint32_t>(hierarchy.size()),
          1u,
          2u,
          8u,
        },
        {1.0f, 1.0f, 1.0f},
        "Tree64Scene");
    assert(chain.update(jobs));
    svrRendering::SparseVoxelTree64SceneBuildChain overflowChain(
        graph,
        passIndex,
        overflowNodePool,
        {
          side,
          1u,
          slotCapacity,
          slotCapacity,
        },
        {
          instances,
          hierarchyBuffer,
          centersBuffer,
          rootsBuffer,
          1u,
          static_cast<uint32_t>(hierarchy.size()),
          1u,
          2u,
          1u,
        },
        {1.0f, 1.0f, 1.0f},
        "Tree64SceneOverflow");
    const SparseVoxelTree64BuildJobGPU overflowJob{
      {0, 0, 0},
      0u,
      77u,
      kSparseVoxelTree64BuildJobValid,
      0u,
      0u,
    };
    assert(overflowChain.update({overflowJob}));
    auto *copyNodes = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
        "Tree64Scene.CopyNodes", passIndex++, nodePool, 0u, uint64_t{nodeCount} * sizeof(SparseVoxelTree64NodeGPU), nodeReadback, 0u, uint64_t{nodeCount} * sizeof(SparseVoxelTree64NodeGPU));
    auto *copyResults = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
        "Tree64Scene.CopyResults", passIndex++, chain.results(), 0u, chunkCount * sizeof(SparseVoxelTree64BuildResultGPU), resultReadback, 0u, chunkCount * sizeof(SparseVoxelTree64BuildResultGPU));
    auto *copyCounters = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
        "Tree64Scene.CopyCounters", passIndex++, chain.sceneCounters(), 0u, sizeof(SparseVoxelTree64SceneCountersGPU), counterReadback, 0u, sizeof(SparseVoxelTree64SceneCountersGPU));
    auto *copyOverflowNodes = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
        "Tree64Scene.CopyOverflowNodes", passIndex++, overflowNodePool, 0u, uint64_t{slotCapacity} * sizeof(SparseVoxelTree64NodeGPU), overflowNodeReadback, 0u, uint64_t{slotCapacity} * sizeof(SparseVoxelTree64NodeGPU));
    auto *copyOverflowResult = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
        "Tree64Scene.CopyOverflowResult", passIndex++, overflowChain.results(), 0u, sizeof(SparseVoxelTree64BuildResultGPU), overflowResultReadback, 0u, sizeof(SparseVoxelTree64BuildResultGPU));
    auto *copyOverflowCounters = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
        "Tree64Scene.CopyOverflowCounters", passIndex++, overflowChain.sceneCounters(), 0u, sizeof(SparseVoxelTree64SceneCountersGPU), overflowCounterReadback, 0u, sizeof(SparseVoxelTree64SceneCountersGPU));
    graph.compile();
    RenderGraph::Frame frame{};
    RenderGraph::Overrides overrides{};
    graph.run(frame, overrides);
    graph.waitFrame(frame);

    std::vector<SparseVoxelTree64NodeGPU> nodes(nodeCount);
    std::array<SparseVoxelTree64BuildResultGPU, chunkCount> buildResults{};
    SparseVoxelTree64SceneCountersGPU counters{};
    graph.bufferRead(
        nodeReadback,
        0u,
        uint64_t{nodeCount} * sizeof(SparseVoxelTree64NodeGPU),
        [&](const void *data)
        {
          std::memcpy(nodes.data(), data, uint64_t{nodeCount} * sizeof(SparseVoxelTree64NodeGPU));
        });
    graph.bufferRead(
        resultReadback,
        0u,
        sizeof(buildResults),
        [&](const void *data)
        {
          std::memcpy(buildResults.data(), data, sizeof(buildResults));
        });
    graph.bufferRead(
        counterReadback,
        0u,
        sizeof(counters),
        [&](const void *data)
        {
          std::memcpy(&counters, data, sizeof(counters));
        });

    for (uint32_t index = 0u; index < chunkCount; ++index)
    {
      assert(buildResults[index].status == kSparseVoxelTree64BuildSuccess);
      assert(buildResults[index].nodeCount == expected[index].nodes.size());
      assert(buildResults[index].nodeSlot == index);
      assert(buildResults[index].contentGeneration == 41u);
      assert(std::memcmp(nodes.data() + uint64_t{index} * slotCapacity, expected[index].nodes.data(), expected[index].nodes.size() * sizeof(SparseVoxelTree64NodeGPU)) == 0);
    }
    assert(counters.queueOverflow == 0u);
    assert(counters.instancesTested == 1u);
    assert(counters.nodesTested == 3u);
    assert(counters.leavesReached == 2u);
    assert(counters.cellsWritten == 8u);

    SparseVoxelTree64BuildResultGPU overflowResult{};
    SparseVoxelTree64SceneCountersGPU overflowCounters{};
    std::vector<SparseVoxelTree64NodeGPU> overflowNodes(slotCapacity);
    graph.bufferRead(
        overflowResultReadback,
        0u,
        sizeof(overflowResult),
        [&](const void *data)
        {
          std::memcpy(&overflowResult, data, sizeof(overflowResult));
        });
    graph.bufferRead(
        overflowCounterReadback,
        0u,
        sizeof(overflowCounters),
        [&](const void *data)
        {
          std::memcpy(&overflowCounters, data, sizeof(overflowCounters));
        });
    graph.bufferRead(
        overflowNodeReadback,
        0u,
        uint64_t{slotCapacity} * sizeof(SparseVoxelTree64NodeGPU),
        [&](const void *data)
        {
          std::memcpy(overflowNodes.data(), data, uint64_t{slotCapacity} * sizeof(SparseVoxelTree64NodeGPU));
        });
    SparseVoxelTree64Chunk fullChunk{};
    assert(buildSparseVoxelTree64(side, std::vector<uint8_t>(side * side * side, 1u), fullChunk, &error));
    assert(overflowCounters.queueOverflow == 1u);
    assert(overflowResult.status == kSparseVoxelTree64BuildSuccess);
    assert(overflowResult.nodeCount == fullChunk.nodes.size());
    assert(std::memcmp(overflowNodes.data(), fullChunk.nodes.data(), fullChunk.nodes.size() * sizeof(SparseVoxelTree64NodeGPU)) == 0);

    std::cout << "TREE64_T5_GPU_COUNTERS instances=" << counters.instancesTested << " nodes=" << counters.nodesTested << " leaves=" << counters.leavesReached << " chunk_splats=" << counters.chunkSplats
              << " cell_writes=" << counters.cellsWritten << '\n';

    // Simulate a resident-page eviction by publishing a hierarchy cut that
    // no longer contains the second leaf. Rebuild only its old chunk from the
    // current cut; the replacement must become exactly empty.
    auto evictedHierarchy = hierarchy;
    evictedHierarchy[0].childCount = 1u;
    graph.bufferWrite(hierarchyBuffer, 0u, sizeof(evictedHierarchy), evictedHierarchy.data());
    SparseVoxelTree64BuildJobGPU evictedJob{
      {1, 0, 0},
      1u,
      42u,
      kSparseVoxelTree64BuildJobValid,
      0u,
      0u,
    };
    assert(chain.update({evictedJob}));
    RenderGraph::Frame evictionFrame{};
    graph.run(evictionFrame, overrides);
    graph.waitFrame(evictionFrame);
    SparseVoxelTree64BuildResultGPU evictionResult{};
    graph.bufferRead(
        resultReadback,
        0u,
        sizeof(evictionResult),
        [&](const void *data)
        {
          std::memcpy(&evictionResult, data, sizeof(evictionResult));
        });
    graph.bufferRead(
        nodeReadback,
        0u,
        uint64_t{nodeCount} * sizeof(SparseVoxelTree64NodeGPU),
        [&](const void *data)
        {
          std::memcpy(nodes.data(), data, uint64_t{nodeCount} * sizeof(SparseVoxelTree64NodeGPU));
        });
    assert(evictionResult.status == kSparseVoxelTree64BuildSuccess);
    assert(evictionResult.nodeSlot == 1u);
    assert(evictionResult.contentGeneration == 42u);
    assert(evictionResult.nodeCount == 1u);
    const auto &emptyRoot = nodes[slotCapacity];
    assert(emptyRoot.populationMaskLo == 0u);
    assert(emptyRoot.populationMaskHi == 0u);

    delete copyOverflowCounters;
    delete copyOverflowResult;
    delete copyOverflowNodes;
    delete copyCounters;
    delete copyResults;
    delete copyNodes;
  }
  graph.deleteBuffer(overflowCounterReadback);
  graph.deleteBuffer(overflowResultReadback);
  graph.deleteBuffer(overflowNodeReadback);
  graph.deleteBuffer(overflowNodePool);
  graph.deleteBuffer(counterReadback);
  graph.deleteBuffer(resultReadback);
  graph.deleteBuffer(nodeReadback);
  graph.deleteBuffer(nodePool);
  graph.deleteBuffer(rootsBuffer);
  graph.deleteBuffer(centersBuffer);
  graph.deleteBuffer(hierarchyBuffer);
  graph.deleteBuffer(instances);
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
  testResidentSceneBuild(rhi);
  std::cout << "Sparse voxel Tree64 T5 resident scene GPU tests passed\n";
  os::Logger::shutdown();
  return 0;
}
