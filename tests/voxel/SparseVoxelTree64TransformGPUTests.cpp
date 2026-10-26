#include "os/Logger.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/SparseVoxelTree64Transforms.hpp"
#include "voxel/svraster/rendering/SparseVoxelTree64SceneBuildChain.hpp"

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
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace
{

using namespace ::rendering;
using namespace voxel::svraster;
namespace svrRendering = voxel::svraster::rendering;

std::array<float, 16> transform(const float tx, const float ty, const float tz, const float sx = 1.0f, const float sy = 1.0f, const float sz = 1.0f)
{
  return {
    sx,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    sy,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    sz,
    0.0f,
    tx,
    ty,
    tz,
    1.0f,
  };
}

std::array<float, 16> rotateZ90(const float tx, const float ty, const float tz)
{
  return {
    0.0f,
    1.0f,
    0.0f,
    0.0f,
    -1.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    1.0f,
    0.0f,
    tx,
    ty,
    tz,
    1.0f,
  };
}

std::array<float, 3> transformPoint(const std::array<float, 16> &matrix, const std::array<float, 3> &point)
{
  return {
    matrix[0] * point[0] + matrix[4] * point[1] + matrix[8] * point[2] + matrix[12],
    matrix[1] * point[0] + matrix[5] * point[1] + matrix[9] * point[2] + matrix[13],
    matrix[2] * point[0] + matrix[6] * point[1] + matrix[10] * point[2] + matrix[14],
  };
}

SparseVoxelAABB transformedCube(const std::array<float, 16> &matrix, const std::array<float, 3> center, const float length)
{
  SparseVoxelAABB bounds{
    {1.0e30f, 1.0e30f, 1.0e30f},
    {-1.0e30f, -1.0e30f, -1.0e30f},
  };
  const float radius = length * 0.5f;
  for (uint32_t corner = 0u; corner < 8u; ++corner)
  {
    const auto world = transformPoint(
        matrix,
        {
          center[0] + ((corner & 1u) != 0u ? radius : -radius),
          center[1] + ((corner & 2u) != 0u ? radius : -radius),
          center[2] + ((corner & 4u) != 0u ? radius : -radius),
        });
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      bounds.minimum[axis] = std::min(bounds.minimum[axis], world[axis]);
      bounds.maximum[axis] = std::max(bounds.maximum[axis], world[axis]);
    }
  }
  return bounds;
}

SparseVoxelInstanceGPU gpuInstance(const std::array<float, 16> &matrix, const SparseVoxelAABB &bounds)
{
  SparseVoxelInstanceGPU instance{};
  instance.modelToWorld = matrix;
  instance.previousModelToWorld = matrix;
  instance.worldBoundsMin = {
    bounds.minimum[0],
    bounds.minimum[1],
    bounds.minimum[2],
    0.0f,
  };
  instance.worldBoundsMax = {
    bounds.maximum[0],
    bounds.maximum[1],
    bounds.maximum[2],
    0.0f,
  };
  return instance;
}

SparseVoxelTree64InstanceTransformRecord record(const uint32_t id, const std::array<float, 16> &matrix, const SparseVoxelAABB &bounds)
{
  return {id, 0u, matrix, bounds};
}

void testTransformedRebakes(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  constexpr uint32_t side = 16u;
  constexpr uint32_t chunkCapacity = 32u;
  constexpr std::array<float, 3> center{2.0f, 2.0f, 2.0f};
  constexpr float length = 1.0f;
  const uint32_t slotCapacity = sparseVoxelTree64MaximumNodeCount(side);
  const uint32_t nodeCount = chunkCapacity * slotCapacity;

  SparseVoxelTree64DirectorySettings directorySettings{
    {64u, 64u, 64u},
    side,
    {1.0f, 1.0f, 1.0f},
    slotCapacity,
    125u,
  };
  SparseVoxelTree64StateGPU state{};
  std::string error;
  assert(makeSparseVoxelTree64DirectoryState(directorySettings, {0.25f, 0.25f, 0.25f}, state, &error));
  state.valid = 1u;

  const SparseVoxelLODNodeGPU hierarchy{0u, 0u, 0u, 0u};
  const SparseVoxelCenterLengthGPU centerLength{center, length};
  const SparseVoxelBLASRootRangeGPU roots{0u, 1u, 0u, 0u};

  RenderGraph graph(&rhi);
  const auto storage = [&](const std::string &name, const uint64_t size)
  {
    return graph.createBuffer({
      .name = name,
      .size = size,
      .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
    });
  };
  const Buffer instances = storage("Tree64Transform.Instances", 2u * sizeof(SparseVoxelInstanceGPU));
  const Buffer hierarchyBuffer = storage("Tree64Transform.Hierarchy", sizeof(hierarchy));
  const Buffer centersBuffer = storage("Tree64Transform.Centers", sizeof(centerLength));
  const Buffer rootsBuffer = storage("Tree64Transform.Roots", sizeof(roots));
  const Buffer nodePool = graph.createBuffer({
    .name = "Tree64Transform.Nodes",
    .size = uint64_t{nodeCount} * sizeof(SparseVoxelTree64NodeGPU),
    .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
  });
  const Buffer nodeReadback = graph.createBuffer({
    .name = "Tree64Transform.NodeReadback",
    .size = uint64_t{nodeCount} * sizeof(SparseVoxelTree64NodeGPU),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  const Buffer resultReadback = graph.createBuffer({
    .name = "Tree64Transform.ResultReadback",
    .size = chunkCapacity * sizeof(SparseVoxelTree64BuildResultGPU),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  const Buffer counterReadback = graph.createBuffer({
    .name = "Tree64Transform.CounterReadback",
    .size = sizeof(SparseVoxelTree64SceneCountersGPU),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  graph.bufferWrite(hierarchyBuffer, 0u, sizeof(hierarchy), const_cast<SparseVoxelLODNodeGPU *>(&hierarchy));
  graph.bufferWrite(centersBuffer, 0u, sizeof(centerLength), const_cast<SparseVoxelCenterLengthGPU *>(&centerLength));
  graph.bufferWrite(rootsBuffer, 0u, sizeof(roots), const_cast<SparseVoxelBLASRootRangeGPU *>(&roots));

  uint32_t passIndex = 0u;
  {
    svrRendering::SparseVoxelTree64SceneBuildChain chain(
        graph,
        passIndex,
        nodePool,
        {
          side,
          chunkCapacity,
          slotCapacity,
          nodeCount,
        },
        {
          instances,
          hierarchyBuffer,
          centersBuffer,
          rootsBuffer,
          2u,
          1u,
          1u,
          1u,
          4u,
        },
        {1.0f, 1.0f, 1.0f},
        "Tree64Transform");
    SparseVoxelTree64BuildJobGPU initialJob{
      {0, 0, 0},
      0u,
      1u,
      kSparseVoxelTree64BuildJobValid,
      0u,
      0u,
    };
    assert(chain.update({initialJob}));
    auto *copyNodes = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
        "Tree64Transform.CopyNodes", passIndex++, nodePool, 0u, uint64_t{nodeCount} * sizeof(SparseVoxelTree64NodeGPU), nodeReadback, 0u, uint64_t{nodeCount} * sizeof(SparseVoxelTree64NodeGPU));
    auto *copyResults = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
        "Tree64Transform.CopyResults", passIndex++, chain.results(), 0u, chunkCapacity * sizeof(SparseVoxelTree64BuildResultGPU), resultReadback, 0u, chunkCapacity * sizeof(SparseVoxelTree64BuildResultGPU));
    auto *copyCounters = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
        "Tree64Transform.CopyCounters", passIndex++, chain.sceneCounters(), 0u, sizeof(SparseVoxelTree64SceneCountersGPU), counterReadback, 0u, sizeof(SparseVoxelTree64SceneCountersGPU));
    graph.compile();

    std::vector<std::array<float, 16>> previousMatrices{
      transform(0.0f, 0.0f, 0.0f),
      transform(0.0f, 0.0f, 0.0f),
    };
    {
      const auto initialBounds = transformedCube(previousMatrices[0], center, length);
      std::array initialInstances{
        gpuInstance(previousMatrices[0], initialBounds),
        gpuInstance(previousMatrices[1], initialBounds),
      };
      graph.bufferWrite(instances, 0u, sizeof(initialInstances), initialInstances.data());
      RenderGraph::Frame warmFrame{};
      RenderGraph::Overrides warmOverrides{};
      graph.run(warmFrame, warmOverrides);
      graph.waitFrame(warmFrame);
    }
    uint32_t generation = 10u;
    const auto run = [&](const std::vector<std::array<float, 16>> &currentMatrices, const std::vector<uint32_t> &stableIds, const char *name)
    {
      std::vector<SparseVoxelTree64InstanceTransformRecord> previousRecords;
      for (uint32_t index = 0u; index < previousMatrices.size(); ++index)
      {
        const auto bounds = transformedCube(previousMatrices[index], center, length);
        previousRecords.push_back(record(index, previousMatrices[index], bounds));
      }
      std::vector<SparseVoxelTree64InstanceTransformRecord> currentRecords;
      std::vector<SparseVoxelAABB> currentBounds;
      std::vector<SparseVoxelInstanceGPU> gpuInstances;
      for (uint32_t index = 0u; index < currentMatrices.size(); ++index)
      {
        const auto bounds = transformedCube(currentMatrices[index], center, length);
        currentRecords.push_back(record(stableIds[index], currentMatrices[index], bounds));
        currentBounds.push_back(bounds);
        gpuInstances.push_back(gpuInstance(currentMatrices[index], bounds));
      }
      std::sort(
          currentRecords.begin(),
          currentRecords.end(),
          [](const auto &left, const auto &right)
          {
            return left.stableInstanceId < right.stableInstanceId;
          });
      SparseVoxelTree64TransformUpdate transformUpdate{};
      assert(sparseVoxelTree64PlanTransformInvalidation(state, previousRecords, currentRecords, false, true, transformUpdate, &error));
      assert(transformUpdate.dirtyChunks.size() <= chunkCapacity);

      std::vector<SparseVoxelTree64BuildJobGPU> jobs;
      for (uint32_t index = 0u; index < transformUpdate.dirtyChunks.size(); ++index)
        jobs.push_back({
          transformUpdate.dirtyChunks[index],
          index,
          generation,
          kSparseVoxelTree64BuildJobValid,
          0u,
          0u,
        });
      if (!gpuInstances.empty())
        graph.bufferWrite(instances, 0u, gpuInstances.size() * sizeof(SparseVoxelInstanceGPU), gpuInstances.data());
      assert(chain.update(jobs, static_cast<uint32_t>(gpuInstances.size()), 1u, 1u));
      RenderGraph::Frame frame{};
      RenderGraph::Overrides overrides{};
      const auto rebuildBegin = std::chrono::steady_clock::now();
      graph.run(frame, overrides);
      graph.waitFrame(frame);
      const auto rebuildElapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - rebuildBegin).count();

      std::vector<SparseVoxelTree64BuildResultGPU> results(chunkCapacity);
      std::vector<SparseVoxelTree64NodeGPU> nodes(nodeCount);
      SparseVoxelTree64SceneCountersGPU counters{};
      graph.bufferRead(
          resultReadback,
          0u,
          results.size() * sizeof(SparseVoxelTree64BuildResultGPU),
          [&](const void *data)
          {
            std::memcpy(results.data(), data, results.size() * sizeof(SparseVoxelTree64BuildResultGPU));
          });
      graph.bufferRead(
          nodeReadback,
          0u,
          nodes.size() * sizeof(SparseVoxelTree64NodeGPU),
          [&](const void *data)
          {
            std::memcpy(nodes.data(), data, nodes.size() * sizeof(SparseVoxelTree64NodeGPU));
          });
      graph.bufferRead(
          counterReadback,
          0u,
          sizeof(counters),
          [&](const void *data)
          {
            std::memcpy(&counters, data, sizeof(counters));
          });

      std::vector<SparseVoxelTree64BuildSourceGPU> sources;
      assert(sparseVoxelTree64MakeBuildSources(state, transformUpdate.dirtyChunks, currentBounds, sources, nullptr, &error));
      for (uint32_t jobIndex = 0u; jobIndex < jobs.size(); ++jobIndex)
      {
        std::vector<SparseVoxelTree64AABB> local;
        for (const auto &source : sources)
          if (source.jobIndex == jobIndex)
            local.push_back({
              {
                source.minimum[0],
                source.minimum[1],
                source.minimum[2],
              },
              {
                source.maximum[0],
                source.maximum[1],
                source.maximum[2],
              },
            });
        SparseVoxelTree64Chunk expected{};
        assert(buildSparseVoxelTree64(side, local, expected, &error));
        assert(results[jobIndex].status == kSparseVoxelTree64BuildSuccess);
        assert(results[jobIndex].nodeCount == expected.nodes.size());
        assert(std::memcmp(nodes.data() + uint64_t{jobIndex} * slotCapacity, expected.nodes.data(), expected.nodes.size() * sizeof(SparseVoxelTree64NodeGPU)) == 0);
      }
      std::cout << "TREE64_T6_GPU variant=" << name << " instances=" << currentMatrices.size() << " dirty_chunks=" << jobs.size() << " nodes_tested=" << counters.nodesTested << " cell_writes=" << counters.cellsWritten
                << " submit_wait_us=" << rebuildElapsed << '\n';
      previousMatrices = currentMatrices;
      ++generation;
      return transformUpdate;
    };

    const auto translated = run(
        {
          transform(17.0f, 0.0f, 0.0f),
          transform(0.0f, 0.0f, 0.0f),
        },
        {0u, 1u},
        "translation_overlap");
    assert(translated.dirtyChunks.size() == 2u);
    const auto rotated = run(
        {
          rotateZ90(20.0f, 0.0f, 0.0f),
          transform(0.0f, 0.0f, 0.0f),
        },
        {0u, 1u},
        "rotation");
    assert(!rotated.dirtyChunks.empty());
    const auto scaledReflected = run(
        {
          transform(20.0f, 4.0f, -4.0f, -2.0f, 0.5f, 3.0f),
          transform(0.0f, 0.0f, 0.0f),
        },
        {0u, 1u},
        "reflection_nonuniform_scale");
    assert(!scaledReflected.dirtyChunks.empty());
    const auto removed = run({transform(0.0f, 0.0f, 0.0f)}, {1u}, "remove_after_move");
    assert(removed.changedInstanceIds == std::vector<uint32_t>{0u});

    delete copyCounters;
    delete copyResults;
    delete copyNodes;
  }
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
  testTransformedRebakes(rhi);
  std::cout << "Sparse voxel Tree64 T6 transformed GPU tests passed\n";
  os::Logger::shutdown();
  return 0;
}
