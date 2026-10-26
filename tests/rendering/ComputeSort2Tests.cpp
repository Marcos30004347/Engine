#include <cassert>
#include <vector>

#include "os/File.hpp"
#include "os/Logger.hpp"

#include "ComputeSortBenchmarkUtils.hpp"

#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"

#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpgpu/RadixSort2Pass.hpp"

using namespace rendering;
using namespace backend;

int main()
{
  os::Logger::start();

  DeviceRequiredLimits limits = {
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
    const std::vector<uint32_t> inputKeys = compute_sort_benchmark::buildDescendingKeys(count);
    const std::vector<uint32_t> inputValues = compute_sort_benchmark::buildAscendingValues(count);

    Buffer keysA = renderGraph->createBuffer(
        BufferInfo{
          .name = "keys.buffer." + std::to_string(count),
          .size = count * sizeof(uint32_t),
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
        });

    Buffer valuesA = renderGraph->createBuffer(
        BufferInfo{
          .name = "values.buffer." + std::to_string(count),
          .size = count * sizeof(uint32_t),
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
        });

    Buffer debugBuffer = renderGraph->createBuffer(
        BufferInfo{
          .name = "Debug.buffer." + std::to_string(count),
          .size = count * sizeof(uint32_t),
          .usage = BufferUsage::BufferUsage_CopyDst | BufferUsage::BufferUsage_Pull,
        });

    renderGraph->bufferWrite(keysA, 0, count * sizeof(uint32_t), (void *)inputKeys.data());
    renderGraph->bufferWrite(valuesA, 0, count * sizeof(uint32_t), (void *)inputValues.data());

    RenderGraph::Frame frame;
    auto *sortPass = renderGraph->registerPass<gpgpu::RadixSort2Pass<uint32_t, uint32_t>>(compute_sort_benchmark::kSortPassName, 0u, count, keysA, valuesA);
    renderGraph->registerPass<gpgpu::CopyBufferPass>("copyRadixSortresultsPass", 1u, keysA, 0u, count * sizeof(uint32_t), debugBuffer, 0u, count * sizeof(uint32_t));
    renderGraph->compile();
    sortPass->ensureDispatchTimerAllocated();
    double warmupGpuTimeMs = 0.0;
    double totalMeasuredGpuTimeMs = 0.0;
    const uint32_t measuredRuns = compute_sort_benchmark::kBenchmarkTotalRuns - compute_sort_benchmark::kBenchmarkWarmupRuns;

    for (uint32_t runIndex = 0; runIndex < compute_sort_benchmark::kBenchmarkTotalRuns; ++runIndex)
    {
      renderGraph->bufferWrite(keysA, 0, count * sizeof(uint32_t), (void *)inputKeys.data());
      renderGraph->bufferWrite(valuesA, 0, count * sizeof(uint32_t), (void *)inputValues.data());

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
          debugBuffer,
          0,
          count * sizeof(uint32_t),
          [count](const void *data)
          {
            compute_sort_benchmark::assertStrictlyIncreasing(static_cast<const uint32_t *>(data), count);
          });
    }

    const double averageGpuTimeMs = measuredRuns > 0u ? (totalMeasuredGpuTimeMs / static_cast<double>(measuredRuns)) : 0.0;
    compute_sort_benchmark::logBenchmarkAverage("RadixSort2Pass", count, warmupGpuTimeMs, averageGpuTimeMs, measuredRuns);

    renderGraph->removePass<gpgpu::RadixSort2Pass<uint32_t, uint32_t>>(compute_sort_benchmark::kSortPassName);
    renderGraph->removePass<gpgpu::CopyBufferPass>("copyRadixSortresultsPass");
    renderGraph->deleteBuffer(keysA);
    renderGraph->deleteBuffer(valuesA);
    renderGraph->deleteBuffer(debugBuffer);
  }

  os::Logger::shutdown();
  return 0;
}
