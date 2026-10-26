#include "os/Logger.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/SparseVoxelTree64.hpp"
#include "voxel/svraster/SparseVoxelTree64Reference.hpp"
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
#include <chrono>
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

constexpr uint32_t kVolumeSide = 64u;

uint32_t cellIndex(const uint32_t side, const uint32_t x, const uint32_t y, const uint32_t z)
{
  return x + side * (y + side * z);
}

bool occupied(const uint32_t x, const uint32_t y, const uint32_t z)
{
  const bool perforatedWall = x == 31u && ((y + 2u * z) & 7u) != 0u;
  const bool beam = y == 17u && z >= 5u && z <= 58u && (x & 3u) != 0u;
  const bool sparse = ((x * 73856093u) ^ (y * 19349663u) ^ (z * 83492791u)) % 997u == 0u;
  return perforatedWall || beam || sparse;
}

SparseVoxelOccupancyDDARayGPU gpuRay(const std::array<float, 3> &origin, const std::array<float, 3> &direction)
{
  return {
    {origin[0], origin[1], origin[2], 192.0f},
    {direction[0], direction[1], direction[2], 0.0f},
  };
}

std::vector<SparseVoxelOccupancyDDARayGPU> rays()
{
  std::vector<SparseVoxelOccupancyDDARayGPU> result{
    gpuRay({-16.0f, 20.5f, 20.5f}, {1.0f, 0.0f, 0.0f}),
    gpuRay({80.0f, 20.5f, 20.5f}, {-1.0f, 0.0f, 0.0f}),
    gpuRay({20.5f, -16.0f, 20.5f}, {0.0f, 1.0f, 0.0f}),
    gpuRay({20.5f, 80.0f, 20.5f}, {0.0f, -1.0f, 0.0f}),
  };
  std::mt19937 random(0x6488u);
  std::uniform_real_distribution<float> origin(-32.0f, 96.0f);
  std::uniform_real_distribution<float> direction(-1.0f, 1.0f);
  for (uint32_t index = 0u; index < 2048u; ++index)
  {
    std::array<float, 3> vector{direction(random), direction(random), direction(random)};
    const float length = std::sqrt(vector[0] * vector[0] + vector[1] * vector[1] + vector[2] * vector[2]);
    if (length < 1.0e-5f)
      vector = {1.0f, 0.0f, 0.0f};
    else
      for (float &component : vector)
        component /= length;
    result.push_back(gpuRay({origin(random), origin(random), origin(random)}, vector));
  }
  return result;
}

struct VariantResult
{
  std::vector<SparseVoxelOccupancyDDAResultGPU> traces;
  double medianMicroseconds = 0.0;
  uint32_t directoryEntries = 0u;
  uint32_t nodes = 0u;
  uint64_t nodePoolBytes = 0u;
};

VariantResult runVariant(::rendering::backend::vulkan::VulkanRHI &rhi, const uint32_t chunkSide, const std::vector<SparseVoxelOccupancyDDARayGPU> &traceRays)
{
  assert(chunkSide == 16u || chunkSide == 64u);
  const uint32_t chunkAxis = kVolumeSide / chunkSide;
  const uint32_t chunkCount = chunkAxis * chunkAxis * chunkAxis;
  const uint32_t slotCapacity = sparseVoxelTree64MaximumNodeCount(chunkSide);
  std::vector<SparseVoxelTree64ChunkGPU> directory(chunkCount);
  std::vector<SparseVoxelTree64NodeGPU> nodePool(uint64_t{chunkCount} * slotCapacity);
  uint32_t totalNodes = 0u;
  for (uint32_t chunkZ = 0u; chunkZ < chunkAxis; ++chunkZ)
    for (uint32_t chunkY = 0u; chunkY < chunkAxis; ++chunkY)
      for (uint32_t chunkX = 0u; chunkX < chunkAxis; ++chunkX)
      {
        std::vector<uint8_t> cells(uint64_t{chunkSide} * chunkSide * chunkSide, 0u);
        for (uint32_t z = 0u; z < chunkSide; ++z)
          for (uint32_t y = 0u; y < chunkSide; ++y)
            for (uint32_t x = 0u; x < chunkSide; ++x)
              cells[cellIndex(chunkSide, x, y, z)] = occupied(chunkX * chunkSide + x, chunkY * chunkSide + y, chunkZ * chunkSide + z) ? 1u : 0u;
        SparseVoxelTree64Chunk tree;
        std::string error;
        assert(buildSparseVoxelTree64(chunkSide, cells, tree, &error));
        const uint32_t index = chunkX + chunkAxis * (chunkY + chunkAxis * chunkZ);
        directory[index] = {
          {static_cast<int32_t>(chunkX), static_cast<int32_t>(chunkY), static_cast<int32_t>(chunkZ)},
          index,
          0u,
          static_cast<uint32_t>(tree.nodes.size()),
          17u,
          kSparseVoxelTree64ChunkValid | (sparseVoxelTree64PopulationMask(tree.nodes.front()) == 0u ? 0u : kSparseVoxelTree64ChunkOccupied),
        };
        std::copy(tree.nodes.begin(), tree.nodes.end(), nodePool.begin() + uint64_t{index} * slotCapacity);
        totalNodes += static_cast<uint32_t>(tree.nodes.size());
      }

  SparseVoxelTree64StateGPU state{};
  state.logicalChunkResolution = {chunkAxis, chunkAxis, chunkAxis};
  state.chunkSide = chunkSide;
  state.originWorldChunk = {0, 0, 0};
  state.valid = 1u;
  state.contentGeneration = 17u;
  state.originCell = {0, 0, 0};
  state.logicalCellResolution = {kVolumeSide, kVolumeSide, kVolumeSide};
  state.cellSize = {1.0f, 1.0f, 1.0f};
  state.worldExtent = {static_cast<float>(kVolumeSide), static_cast<float>(kVolumeSide), static_cast<float>(kVolumeSide)};

  RenderGraph graph(&rhi);
  const auto upload = [&](const std::string &name, const uint64_t size, const BufferUsage usage)
  {
    return graph.createBuffer({
      .name = name,
      .size = size,
      .usage = usage | BufferUsage::BufferUsage_Push,
      .frameLocal = true,
    });
  };
  const std::string prefix = "Tree64T8.Side" + std::to_string(chunkSide);
  const Buffer stateBuffer = upload(prefix + ".State", sizeof(state), BufferUsage::BufferUsage_Storage);
  const Buffer chunkBuffer = upload(prefix + ".Chunks", directory.size() * sizeof(directory[0]), BufferUsage::BufferUsage_Storage);
  const Buffer nodeBuffer = upload(prefix + ".Nodes", nodePool.size() * sizeof(nodePool[0]), BufferUsage::BufferUsage_Storage);
  const Buffer rayBuffer = upload(prefix + ".Rays", traceRays.size() * sizeof(traceRays[0]), BufferUsage::BufferUsage_Storage);
  const Buffer resultBuffer = graph.createBuffer({
    .name = prefix + ".Results",
    .size = traceRays.size() * sizeof(SparseVoxelOccupancyDDAResultGPU),
    .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
  });
  const Buffer readback = graph.createBuffer({
    .name = prefix + ".Readback",
    .size = traceRays.size() * sizeof(SparseVoxelOccupancyDDAResultGPU),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  graph.bufferWrite(stateBuffer, 0u, sizeof(state), &state);
  graph.bufferWrite(chunkBuffer, 0u, directory.size() * sizeof(directory[0]), directory.data());
  graph.bufferWrite(nodeBuffer, 0u, nodePool.size() * sizeof(nodePool[0]), nodePool.data());
  graph.bufferWrite(rayBuffer, 0u, traceRays.size() * sizeof(traceRays[0]), const_cast<SparseVoxelOccupancyDDARayGPU *>(traceRays.data()));

  uint32_t passIndex = 0u;
  auto *trace = graph.registerPass<svrRendering::SparseVoxelTree64TracePass>(
      prefix + ".Trace",
      passIndex++,
      svrRendering::SparseVoxelTree64TracePass::Buffers{stateBuffer, chunkBuffer, nodeBuffer, rayBuffer, resultBuffer},
      svrRendering::SparseVoxelTree64TracePass::Capacities{chunkCount, static_cast<uint32_t>(nodePool.size()), slotCapacity, static_cast<uint32_t>(traceRays.size())});
  assert(trace->update(static_cast<uint32_t>(traceRays.size()), 4096u));
  auto *copy = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      prefix + ".Copy", passIndex++, resultBuffer, 0u, traceRays.size() * sizeof(SparseVoxelOccupancyDDAResultGPU), readback, 0u, traceRays.size() * sizeof(SparseVoxelOccupancyDDAResultGPU));
  graph.compile();

  RenderGraph::Overrides overrides{};
  RenderGraph::Frame warmup{};
  graph.run(warmup, overrides);
  graph.waitFrame(warmup);

  VariantResult result;
  result.traces.resize(traceRays.size());
  graph.bufferRead(
      readback,
      0u,
      result.traces.size() * sizeof(result.traces[0]),
      [&](const void *data)
      {
        std::memcpy(result.traces.data(), data, result.traces.size() * sizeof(result.traces[0]));
      });

  constexpr uint32_t repetitions = 3u;
  constexpr uint32_t frames = 32u;
  std::array<double, repetitions> samples{};
  for (double &sample : samples)
  {
    const auto begin = std::chrono::steady_clock::now();
    for (uint32_t frameIndex = 0u; frameIndex < frames; ++frameIndex)
    {
      RenderGraph::Frame frame{};
      graph.run(frame, overrides);
      graph.waitFrame(frame);
    }
    sample = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count();
  }
  std::sort(samples.begin(), samples.end());
  result.medianMicroseconds = samples[repetitions / 2u];
  result.directoryEntries = chunkCount;
  result.nodes = totalNodes;
  result.nodePoolBytes = nodePool.size() * sizeof(nodePool[0]);

  delete copy;
  delete trace;
  graph.deleteBuffer(readback);
  graph.deleteBuffer(resultBuffer);
  graph.deleteBuffer(rayBuffer);
  graph.deleteBuffer(nodeBuffer);
  graph.deleteBuffer(chunkBuffer);
  graph.deleteBuffer(stateBuffer);
  return result;
}

void assertEquivalent(const VariantResult &side16, const VariantResult &side64)
{
  assert(side16.traces.size() == side64.traces.size());
  for (uint32_t index = 0u; index < side16.traces.size(); ++index)
  {
    const auto &left = side16.traces[index];
    const auto &right = side64.traces[index];
    assert(left.status == right.status);
    if (left.status == static_cast<uint32_t>(SparseVoxelTree64TraceStatus::Hit))
    {
      assert(left.globalCell == right.globalCell);
      assert(std::abs(left.distance - right.distance) < 2.0e-4f);
    }
  }
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

  const auto traceRays = rays();
  const auto side16 = runVariant(rhi, 16u, traceRays);
  const auto side64 = runVariant(rhi, 64u, traceRays);
  assertEquivalent(side16, side64);
  constexpr uint32_t frames = 32u;
  for (const auto &[side, result] : std::array<std::pair<uint32_t, const VariantResult *>, 2u>{{{16u, &side16}, {64u, &side64}}})
    std::cout << "TREE64_T8_GPU specialization=chunk_side"
              << " side=" << side << " rays=" << traceRays.size() << " frames=" << frames << " median_submit_wait_us=" << result->medianMicroseconds << " directory_entries=" << result->directoryEntries
              << " populated_nodes=" << result->nodes << " node_pool_bytes=" << result->nodePoolBytes << '\n';
  std::cout << "Sparse voxel Tree64 T8 specialization GPU tests passed\n";
  os::Logger::shutdown();
  return 0;
}
