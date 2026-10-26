#include "os/Logger.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "voxel/svraster/SparseVoxelTree64.hpp"
#include "voxel/svraster/rendering/SparseVoxelTree64PublishPass.hpp"

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

void testPublication(::rendering::backend::vulkan::VulkanRHI &rhi, const bool successful, const bool publishEmpty = false)
{
  constexpr uint32_t side = 16u;
  const uint32_t slotCapacity = sparseVoxelTree64MaximumNodeCount(side);
  const uint32_t nodeCount = 2u * slotCapacity;

  SparseVoxelTree64StateGPU targetState{};
  targetState.logicalChunkResolution = {2u, 1u, 1u};
  targetState.chunkSide = side;
  targetState.valid = 1u;
  targetState.contentGeneration = 5u;
  targetState.logicalCellResolution = {32u, 16u, 16u};
  targetState.cellSize = {1.0f, 1.0f, 1.0f};
  targetState.worldExtent = {32.0f, 16.0f, 16.0f};

  const SparseVoxelTree64ChunkGPU retained{
    {0, 0, 0},
    0u,
    0u,
    slotCapacity,
    4u,
    kSparseVoxelTree64ChunkValid | kSparseVoxelTree64ChunkOccupied,
  };
  std::vector<SparseVoxelTree64ChunkGPU> initialDirectory{
    retained,
    {
      {777, 777, 777},
      99u,
      99u,
      99u,
      99u,
      0u,
    },
  };
  std::vector<SparseVoxelTree64NodeGPU> nodes(nodeCount);
  nodes[slotCapacity] = {2u, 0xFFFFFFFFu, 0xFFFFFFFFu};
  const SparseVoxelTree64BuildResultGPU result{
    successful ? kSparseVoxelTree64BuildSuccess : kSparseVoxelTree64BuildOverflow,
    slotCapacity,
    0u,
    1u,
    5u,
    successful ? 0u : kSparseVoxelTree64BuildOverflowPreflight,
    1u,
    0u,
  };
  const SparseVoxelTree64PublishRecordGPU record{
    {1, 0, 0},
    1u,
    1u,
    5u,
    0u,
    publishEmpty ? kSparseVoxelTree64PublishRecordEmpty : 0u,
  };

  RenderGraph graph(&rhi);
  const auto upload = [&](const std::string &name, const uint64_t size, const BufferUsage usage)
  {
    return graph.createBuffer({
      .name = name,
      .size = size,
      .usage = usage | BufferUsage::BufferUsage_Push,
    });
  };
  const Buffer resultBuffer = upload("Tree64Publish.Results", sizeof(result), BufferUsage::BufferUsage_Storage);
  const Buffer nodeBuffer = upload("Tree64Publish.Nodes", uint64_t{nodes.size()} * sizeof(SparseVoxelTree64NodeGPU), BufferUsage::BufferUsage_Storage);
  const Buffer recordBuffer = upload("Tree64Publish.Records", sizeof(record), BufferUsage::BufferUsage_Storage);
  const Buffer targetStateBuffer = upload("Tree64Publish.TargetState", sizeof(targetState), BufferUsage::BufferUsage_Storage);
  const Buffer directoryBuffer = upload("Tree64Publish.Directory", uint64_t{initialDirectory.size()} * sizeof(SparseVoxelTree64ChunkGPU), BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer committedStateBuffer = upload("Tree64Publish.CommittedState", sizeof(targetState), BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer directoryReadback = graph.createBuffer({
    .name = "Tree64Publish.DirectoryReadback",
    .size = uint64_t{initialDirectory.size()} * sizeof(SparseVoxelTree64ChunkGPU),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  const Buffer stateReadback = graph.createBuffer({
    .name = "Tree64Publish.StateReadback",
    .size = sizeof(targetState),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  graph.bufferWrite(resultBuffer, 0u, sizeof(result), const_cast<SparseVoxelTree64BuildResultGPU *>(&result));
  graph.bufferWrite(nodeBuffer, 0u, uint64_t{nodes.size()} * sizeof(SparseVoxelTree64NodeGPU), nodes.data());
  graph.bufferWrite(recordBuffer, 0u, sizeof(record), const_cast<SparseVoxelTree64PublishRecordGPU *>(&record));
  graph.bufferWrite(targetStateBuffer, 0u, sizeof(targetState), &targetState);
  graph.bufferWrite(directoryBuffer, 0u, uint64_t{initialDirectory.size()} * sizeof(SparseVoxelTree64ChunkGPU), initialDirectory.data());

  uint32_t passIndex = 0u;
  auto *publish = graph.registerPass<svrRendering::SparseVoxelTree64PublishPass>(
      "Tree64Publish.Publish",
      passIndex++,
      svrRendering::SparseVoxelTree64PublishPass::Buffers{
        resultBuffer,
        nodeBuffer,
        recordBuffer,
        targetStateBuffer,
        directoryBuffer,
        committedStateBuffer,
      },
      svrRendering::SparseVoxelTree64PublishPass::Capacities{
        1u,
        1u,
        2u,
        slotCapacity,
        nodeCount,
      });
  assert(publish->update(1u));
  auto *copyDirectory = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "Tree64Publish.CopyDirectory",
      passIndex++,
      directoryBuffer,
      0u,
      uint64_t{initialDirectory.size()} * sizeof(SparseVoxelTree64ChunkGPU),
      directoryReadback,
      0u,
      uint64_t{initialDirectory.size()} * sizeof(SparseVoxelTree64ChunkGPU));
  auto *copyState = graph.registerPass<::rendering::gpgpu::CopyBufferPass>("Tree64Publish.CopyState", passIndex++, committedStateBuffer, 0u, sizeof(targetState), stateReadback, 0u, sizeof(targetState));

  graph.compile();
  RenderGraph::Frame frame{};
  RenderGraph::Overrides overrides{};
  graph.run(frame, overrides);
  graph.waitFrame(frame);
  std::vector<SparseVoxelTree64ChunkGPU> actualDirectory(2u);
  SparseVoxelTree64StateGPU actualState{};
  graph.bufferRead(
      directoryReadback,
      0u,
      uint64_t{actualDirectory.size()} * sizeof(SparseVoxelTree64ChunkGPU),
      [&](const void *data)
      {
        std::memcpy(actualDirectory.data(), data, uint64_t{actualDirectory.size()} * sizeof(SparseVoxelTree64ChunkGPU));
      });
  graph.bufferRead(
      stateReadback,
      0u,
      sizeof(actualState),
      [&](const void *data)
      {
        std::memcpy(&actualState, data, sizeof(actualState));
      });
  assert(std::memcmp(&actualState, &targetState, sizeof(targetState)) == 0);
  assert(std::memcmp(&actualDirectory[0], &retained, sizeof(retained)) == 0);
  assert(actualDirectory[1].worldChunk == (std::array<int32_t, 3>{1, 0, 0}));
  assert(actualDirectory[1].contentGeneration == 5u);
  assert((actualDirectory[1].flags & kSparseVoxelTree64ChunkValid) != 0u);
  if (publishEmpty)
  {
    assert(actualDirectory[1].nodeSlot == 1u);
    assert(actualDirectory[1].nodeCount == 0u);
    assert((actualDirectory[1].flags & kSparseVoxelTree64ChunkOccupied) == 0u);
    assert((actualDirectory[1].flags & kSparseVoxelTree64ChunkUnknown) == 0u);
  }
  else if (successful)
  {
    assert(actualDirectory[1].nodeSlot == 1u);
    assert(actualDirectory[1].nodeCount == slotCapacity);
    assert((actualDirectory[1].flags & kSparseVoxelTree64ChunkOccupied) != 0u);
    assert((actualDirectory[1].flags & kSparseVoxelTree64ChunkUnknown) == 0u);
  }
  else
  {
    assert(actualDirectory[1].nodeCount == 0u);
    assert((actualDirectory[1].flags & kSparseVoxelTree64ChunkUnknown) != 0u);
  }

  delete copyState;
  delete copyDirectory;
  delete publish;
  graph.deleteBuffer(stateReadback);
  graph.deleteBuffer(directoryReadback);
  graph.deleteBuffer(committedStateBuffer);
  graph.deleteBuffer(directoryBuffer);
  graph.deleteBuffer(targetStateBuffer);
  graph.deleteBuffer(recordBuffer);
  graph.deleteBuffer(nodeBuffer);
  graph.deleteBuffer(resultBuffer);
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
  ::rendering::backend::vulkan::VulkanRHI rhi(::rendering::backend::vulkan::Vulkan_1_2, limits, ::rendering::DeviceFeatures::DeviceFeatures_Compute, {});
  std::vector<VkSurfaceKHR> surfaces;
  rhi.init(surfaces);
  testPublication(rhi, true);
  testPublication(rhi, false);
  testPublication(rhi, false, true);
  std::cout << "Sparse voxel Tree64 T4 GPU publication tests passed\n";
  os::Logger::shutdown();
  return 0;
}
