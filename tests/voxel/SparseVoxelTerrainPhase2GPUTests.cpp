#include "os/Logger.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "voxel/svraster/rendering/TerrainGPUBuildSyntheticChain.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#undef NDEBUG
#include <cassert>

namespace
{

using namespace ::rendering;
using namespace voxel::svraster;
namespace svrRendering = voxel::svraster::rendering;

TerrainGPUDispatch work(const uint64_t dispatchId, const uint64_t jobId,
    const TerrainGPUBuildStage stage, const int32_t chunkX)
{
  TerrainGPUDispatch result{};
  result.dispatchId = dispatchId;
  result.jobId = jobId;
  result.key.bundleSignature = 0xABCD01234567ull;
  result.key.chunkX = chunkX;
  result.key.chunkZ = -chunkX;
  result.key.internalLOD = 1u;
  result.key.generatorRevision = 13u;
  result.key.terrainSlot = static_cast<uint32_t>(jobId);
  result.key.expectedTerrainGeneration = 7u;
  result.stage = stage;
  result.cursor = 16u;
  result.workUnits = 4u;
  result.estimatedMicroseconds = 50u;
  return result;
}

void testSyntheticChain(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  constexpr uint32_t capacity = 4u;
  constexpr uint32_t recordCount = capacity *
      svrRendering::TerrainGPUBuildSyntheticChain::kStageCount;
  RenderGraph graph(&rhi);
  const Buffer readback = graph.createBuffer({
    .name = "TerrainGPUBuild.Phase2.Readback",
    .size = recordCount *
        sizeof(svrRendering::TerrainGPUSyntheticResultGPU),
    .usage = BufferUsage::BufferUsage_Pull |
        BufferUsage::BufferUsage_CopyDst,
  });
  uint32_t passIndex = 0u;
  {
    svrRendering::TerrainGPUBuildSyntheticChain chain(
        graph, passIndex, capacity);
    TerrainGPUFrameSchedule schedule{};
    schedule.dispatches = {
      work(11u, 101u, TerrainGPUBuildStage::SampleColumns, -2),
      work(12u, 102u, TerrainGPUBuildStage::SampleColumns, 3),
      work(13u, 103u, TerrainGPUBuildStage::Validate, 8),
    };
    assert(chain.update(schedule));
    auto *copy = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
        "TerrainGPUBuild.Phase2.Copy", passIndex++, chain.results(), 0u,
        recordCount * sizeof(svrRendering::TerrainGPUSyntheticResultGPU),
        readback, 0u,
        recordCount * sizeof(svrRendering::TerrainGPUSyntheticResultGPU));
    graph.compile();
    RenderGraph::Frame frame{};
    RenderGraph::Overrides overrides{};
    graph.run(frame, overrides);
    graph.waitFrame(frame);
    std::vector<svrRendering::TerrainGPUSyntheticResultGPU> results(
        recordCount);
    graph.bufferRead(readback, 0u,
        results.size() * sizeof(results.front()), [&](const void *data)
        {
          std::memcpy(results.data(), data,
              results.size() * sizeof(results.front()));
        });
    assert(results[0].dispatchIdLow == 11u && results[0].completed == 1u);
    assert(results[1].dispatchIdLow == 12u && results[1].completed == 1u);
    const uint32_t validateOffset = 9u * capacity;
    assert(results[validateOffset].dispatchIdLow == 13u &&
        results[validateOffset].completed == 1u);
    assert(results[0].checksum != 0u);
    assert(results[1].checksum != results[0].checksum);
    assert(results[validateOffset].checksum != 0u);
    delete copy;
  }
  graph.deleteBuffer(readback);
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
  testSyntheticChain(rhi);
  std::cout << "GPU terrain Phase 2 synthetic graph tests passed\n";
  os::Logger::shutdown();
  return 0;
}
