#include <vector>

#include "os/File.hpp"
#include "os/Logger.hpp"

#include "ComputeSortBenchmarkUtils.hpp"

#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpgpu/RadixSortPass.hpp"

#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"

using namespace rendering;
using namespace backend;

int main()
{
  os::Logger::start();

  DeviceRequiredLimits limits = (DeviceRequiredLimits){
    .minimumMemory = 0,
    .minimumComputeSharedMemory = 0,
    .minimumComputeWorkGroupInvocations = 0,
  };

  DeviceFeatures features = DeviceFeatures::DeviceFeatures_Compute | DeviceFeatures::DeviceFeatures_Subgroup_Basic | DeviceFeatures::DeviceFeatures_Subgroup_Shuffle |
                            DeviceFeatures::DeviceFeatures_Timestamp;

  vulkan::VulkanRHI *rhi = new vulkan::VulkanRHI(vulkan::Vulkan_1_2, limits, features, {});

  auto surfaces = std::vector<VkSurfaceKHR>();
  rhi->init(surfaces);

  RenderGraph *renderGraph = new RenderGraph(rhi);

  RenderGraph::Overrides overrides;

  for (const uint32_t count : compute_sort_benchmark::buildElementCounts())
  {
    const std::vector<uint32_t> data = compute_sort_benchmark::buildDescendingKeys(count);
    const uint32_t paddedSize = gpgpu::RadixSortPass<uint32_t, uint32_t>::keysBufferSize(count);

    Buffer keys = renderGraph->createBuffer(
        BufferInfo{
          .name = "Keys.buffer." + std::to_string(count),
          .size = paddedSize * sizeof(uint32_t),
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
        });

    Buffer payload = renderGraph->createBuffer(
        BufferInfo{
          .name = "Payload.buffer." + std::to_string(count),
          .size = paddedSize * sizeof(uint32_t),
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
        });

    Buffer debug = renderGraph->createBuffer(
        BufferInfo{
          .name = "Debug.buffer." + std::to_string(count),
          .size = count * sizeof(uint32_t),
          .usage = BufferUsage::BufferUsage_CopyDst | BufferUsage::BufferUsage_Pull,
        });

    renderGraph->bufferWrite(keys, 0, count * sizeof(uint32_t), (void *)data.data());
    renderGraph->bufferWrite(payload, 0, count * sizeof(uint32_t), (void *)data.data());

    RenderGraph::Frame frame;
    auto *sortPass = renderGraph->registerPass<gpgpu::RadixSortPass<uint32_t, uint32_t>>(compute_sort_benchmark::kSortPassName, 0u, count, keys, payload);
    renderGraph->registerPass<gpgpu::CopyBufferPass>("copyRadixSortresultsPass", 1u, keys, 0u, count * sizeof(uint32_t), debug, 0u, count * sizeof(uint32_t));

    renderGraph->compile();
    sortPass->ensureDispatchTimerAllocated();
    double warmupGpuTimeMs = 0.0;
    double totalMeasuredGpuTimeMs = 0.0;
    const uint32_t measuredRuns = compute_sort_benchmark::kBenchmarkTotalRuns - compute_sort_benchmark::kBenchmarkWarmupRuns;

    for (uint32_t runIndex = 0; runIndex < compute_sort_benchmark::kBenchmarkTotalRuns; ++runIndex)
    {
      renderGraph->bufferWrite(keys, 0, count * sizeof(uint32_t), (void *)data.data());
      renderGraph->bufferWrite(payload, 0, count * sizeof(uint32_t), (void *)data.data());

      frame = RenderGraph::Frame{};
      renderGraph->run(frame, overrides);
      renderGraph->waitFrame(frame);

      const double gpuTimeMs = renderGraph->readTimer(sortPass->getDispatchTimer());
      if (runIndex < compute_sort_benchmark::kBenchmarkWarmupRuns)
      {
        warmupGpuTimeMs = gpuTimeMs;
      }
      else
      {
        totalMeasuredGpuTimeMs += gpuTimeMs;
      }

      renderGraph->bufferRead(
          debug,
          0,
          count * sizeof(uint32_t),
          [count](const void *resultData)
          {
            compute_sort_benchmark::assertStrictlyIncreasing(static_cast<const uint32_t *>(resultData), count);
          });
    }

    const double averageGpuTimeMs = measuredRuns > 0u ? (totalMeasuredGpuTimeMs / static_cast<double>(measuredRuns)) : 0.0;
    compute_sort_benchmark::logBenchmarkAverage("RadixSortPass", count, warmupGpuTimeMs, averageGpuTimeMs, measuredRuns);

    renderGraph->removePass<gpgpu::RadixSortPass<uint32_t, uint32_t>>(compute_sort_benchmark::kSortPassName);
    renderGraph->removePass<gpgpu::CopyBufferPass>("copyRadixSortresultsPass");
    renderGraph->deleteBuffer(keys);
    renderGraph->deleteBuffer(payload);
    renderGraph->deleteBuffer(debug);
  }

  os::Logger::shutdown();

  return 0;
}
