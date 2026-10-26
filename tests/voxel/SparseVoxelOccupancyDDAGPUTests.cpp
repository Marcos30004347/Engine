#include "os/Logger.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "voxel/svraster/SparseVoxelOccupancyDDA.hpp"
#include "voxel/svraster/rendering/SparseVoxelOccupancyDDAPass.hpp"
#include "voxel/svraster/rendering/SparseVoxelOccupancyMipChain.hpp"

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

SparseVoxelOccupancyDDARayGPU gpuRay(
    const SparseVoxelOccupancyRay &ray)
{
  return {
    {ray.origin[0], ray.origin[1], ray.origin[2],
     static_cast<float>(ray.maximumDistance)},
    {ray.direction[0], ray.direction[1], ray.direction[2], 0.0f},
  };
}

void testGPUHierarchy(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  SparseVoxelOccupancySettings settings{};
  settings.worldExtent = {16.0f, 16.0f, 16.0f};
  settings.logicalResolution = {16u, 16u, 16u};
  settings.memoryCeilingBytes = 1u << 20u;

  const SparseVoxelAABB oldBounds{
    {-2.9f, -0.9f, -0.9f},
    {-2.1f, -0.1f, -0.1f},
  };
  const SparseVoxelAABB newBounds{
    {2.1f, 0.1f, 0.1f},
    {2.9f, 0.9f, 0.9f},
  };
  std::vector<SparseVoxelAABB> sources{oldBounds};
  SparseVoxelOccupancyVolume volume;
  std::string error;
  assert(volume.configure(settings, {}, &error));
  assert(volume.update({0.25f, 0.25f, 0.25f}, 1u, sources, &error));

  std::vector<SparseVoxelOccupancyRay> rays{
    {{-20.0f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, 50.0},
    {{20.0f, -0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, 50.0},
    {{-2.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, 50.0},
    {{-20.0f, 4.5f, 4.5f}, {1.0f, 0.0f, 0.0f}, 50.0},
    {{0.5f, -20.0f, 0.5f}, {0.0f, 1.0f, 0.0f}, 50.0},
    {{-20.0f, -20.0f, -20.0f}, {1.0f, 1.0f, 1.0f}, 50.0},
    {{-20.0f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, 10.0},
    {{8.0f, -0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, 50.0},
    // Camera-primary rays commonly start on the world-zero X/Y cell
    // boundaries with tiny signed perspective components. A subnormal
    // boundary bias may be flushed to zero by the GPU and must not stall the
    // hierarchical traversal at distance zero.
    {{0.0f, 0.0f, 3.25f},
     {0.002406f, -0.002406f, -0.999994f},
     64.0},
    {{0.0f, 0.0f, 3.25f},
     {-0.002406f, 0.002406f, -0.999994f},
     64.0},
  };
  std::mt19937 random(0xDDA29u);
  std::uniform_real_distribution<float> originDistribution(-24.0f, 24.0f);
  std::uniform_real_distribution<float> directionDistribution(-1.0f, 1.0f);
  for (uint32_t rayIndex = 0u; rayIndex < 256u; ++rayIndex)
  {
    std::array<float, 3> direction{
      directionDistribution(random),
      directionDistribution(random),
      directionDistribution(random),
    };
    const float length = std::sqrt(
        direction[0] * direction[0] +
        direction[1] * direction[1] +
        direction[2] * direction[2]);
    if (length < 1.0e-4f)
      direction = {1.0f, 0.0f, 0.0f};
    else
      for (float &component : direction)
        component /= length;
    rays.push_back({
      {originDistribution(random), originDistribution(random),
       originDistribution(random)},
      direction,
      64.0,
    });
  }
  std::vector<SparseVoxelOccupancyDDARayGPU> gpuRays;
  gpuRays.reserve(rays.size());
  for (const SparseVoxelOccupancyRay &ray : rays)
    gpuRays.push_back(gpuRay(ray));

  RenderGraph graph(&rhi);
  uint32_t passIndex = 0u;
  svrRendering::SparseVoxelOccupancyMipChain chain(
      graph,
      passIndex,
      volume,
      sources,
      1u,
      "g29.Occupancy");
  const Buffer rayBuffer = graph.createBuffer({
    .name = "g29.Rays",
    .size = gpuRays.size() * sizeof(SparseVoxelOccupancyDDARayGPU),
    .usage = BufferUsage::BufferUsage_Storage |
        BufferUsage::BufferUsage_Push,
    .frameLocal = true,
  });
  const Buffer resultBuffer = graph.createBuffer({
    .name = "g29.Results",
    .size = gpuRays.size() * sizeof(SparseVoxelOccupancyDDAResultGPU),
    .usage = BufferUsage::BufferUsage_Storage |
        BufferUsage::BufferUsage_CopySrc,
  });
  const Buffer readback = graph.createBuffer({
    .name = "g29.Readback",
    .size = gpuRays.size() * sizeof(SparseVoxelOccupancyDDAResultGPU),
    .usage = BufferUsage::BufferUsage_Pull |
        BufferUsage::BufferUsage_CopyDst,
  });
  graph.bufferWrite(
      rayBuffer,
      0u,
      gpuRays.size() * sizeof(SparseVoxelOccupancyDDARayGPU),
      gpuRays.data());

  auto *ddaPass =
      graph.registerPass<svrRendering::SparseVoxelOccupancyDDAPass>(
          "g29.Trace",
          passIndex++,
          chain.occupancyTexture(),
          svrRendering::SparseVoxelOccupancyDDAPass::Buffers{
            chain.baseState(),
            chain.pyramidState(),
            chain.levelDescriptors(),
            chain.mipWords(),
            rayBuffer,
            resultBuffer,
          },
          svrRendering::SparseVoxelOccupancyDDAPass::Capacities{
            static_cast<uint32_t>(rays.size()),
            static_cast<uint32_t>(volume.mipLevels().size()),
            volume.pyramidLayout().wordCount,
          });
  assert(ddaPass->update(static_cast<uint32_t>(rays.size()), 4096u));
  auto *copy = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "g29.CopyResults",
      passIndex++,
      resultBuffer,
      0u,
      gpuRays.size() * sizeof(SparseVoxelOccupancyDDAResultGPU),
      readback,
      0u,
      gpuRays.size() * sizeof(SparseVoxelOccupancyDDAResultGPU));
  graph.compile();

  const auto validate = [&](const bool expectMip0Fallback)
  {
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
          std::memcpy(
              actual.data(),
              data,
              actual.size() * sizeof(SparseVoxelOccupancyDDAResultGPU));
        });
    for (uint32_t rayIndex = 0u; rayIndex < rays.size(); ++rayIndex)
    {
      const SparseVoxelOccupancyTraceResult expected =
          traceSparseVoxelOccupancyMip0(volume, rays[rayIndex]);
      if (actual[rayIndex].status != static_cast<uint32_t>(expected.status))
        std::cerr << "DDA status mismatch ray " << rayIndex
                  << ": expected "
                  << static_cast<uint32_t>(expected.status)
                  << ", actual " << actual[rayIndex].status
                  << ", queries " << actual[rayIndex].queryCount
                  << ", cell (" << actual[rayIndex].globalCell[0] << ", "
                  << actual[rayIndex].globalCell[1] << ", "
                  << actual[rayIndex].globalCell[2] << ")\n";
      assert(
          actual[rayIndex].status ==
          static_cast<uint32_t>(expected.status));
      assert(actual[rayIndex].contentGeneration ==
             volume.state().contentGeneration);
      if (expectMip0Fallback)
        assert(actual[rayIndex].highestLevel == 0u);
      assert(actual[rayIndex].queryCount > 0u ||
             expected.status == SparseVoxelOccupancyTraceStatus::Miss);
      if (expected.hit())
        assert(actual[rayIndex].globalCell == expected.globalCell);
    }
  };

  validate(false);
  SparseVoxelOccupancyPyramidGPU invalidPyramid =
      chain.publishedState();
  invalidPyramid.valid = 0u;
  graph.bufferWrite(
      chain.stagedPyramidState(),
      0u,
      sizeof(invalidPyramid),
      &invalidPyramid);
  validate(true);

  assert(volume.update({2.25f, -1.75f, 1.25f}, 1u, sources, &error));
  assert(chain.update(volume, sources, &error));
  validate(false);

  sources = {newBounds};
  assert(
      volume.update(
          {2.25f, -1.75f, 1.25f},
          2u,
          sources,
          {oldBounds, newBounds},
          &error));
  assert(volume.state().fullRebuild == 0u);
  assert(chain.update(volume, sources, &error));
  validate(false);

  delete copy;
  delete ddaPass;
  graph.deleteBuffer(readback);
  graph.deleteBuffer(resultBuffer);
  graph.deleteBuffer(rayBuffer);
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
  ::rendering::backend::vulkan::VulkanRHI rhi(
      ::rendering::backend::vulkan::Vulkan_1_2,
      limits,
      ::rendering::DeviceFeatures::DeviceFeatures_Compute,
      {});
  std::vector<VkSurfaceKHR> surfaces;
  rhi.init(surfaces);
  testGPUHierarchy(rhi);
  std::cout << "Sparse voxel occupancy G29 GPU DDA tests passed\n";
  os::Logger::shutdown();
  return 0;
}
