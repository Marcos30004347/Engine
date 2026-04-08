#include <cassert>
#include <iostream>

#include "os/File.hpp"
#include "os/Logger.hpp"

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

  uint32_t count = 1024 * 1024; 
  uint32_t *inputKeys = new uint32_t[count];
  uint32_t *inputValues = new uint32_t[count];

  for (uint32_t i = 0; i < count; i++)
  {
    inputKeys[i] = count - i;
    inputValues[i] = i;
  }

  Buffer keysA = renderGraph->createBuffer(
      BufferInfo{
        .name = "keys.buffer",
        .size = count * sizeof(uint32_t),
        .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
      });

  Buffer valuesA = renderGraph->createBuffer(
      BufferInfo{
        .name = "values.buffer",
        .size = count * sizeof(uint32_t),
        .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
      });

  Buffer debugBuffer = renderGraph->createBuffer(
      BufferInfo{
        .name = "Debug.buffer",
        .size = count * sizeof(uint32_t),
        .usage = BufferUsage::BufferUsage_CopyDst | BufferUsage::BufferUsage_Pull,
      });

  renderGraph->bufferWrite(keysA, 0, count * sizeof(uint32_t), (void **)inputKeys);
  renderGraph->bufferWrite(valuesA, 0, count * sizeof(uint32_t), (void **)inputValues);

  RenderGraph::Frame frame;
  RenderGraph::Overrides overrides;

  renderGraph->registerPass<gpgpu::RadixSort2Pass<uint32_t, uint32_t>>("radixSortPass", 0, count, keysA, valuesA);
  renderGraph->registerPass<gpgpu::CopyBufferPass>("copyRadixSortresultsPass", 1, keysA, 0, count * sizeof(uint32_t), debugBuffer, 0, count * sizeof(uint32_t));
  renderGraph->compile();
  renderGraph->run(frame, overrides);
  renderGraph->waitFrame(frame);
  renderGraph->removePass<gpgpu::RadixSort2Pass<uint32_t, uint32_t>>("radixSortPass");
  renderGraph->removePass<gpgpu::CopyBufferPass>("copyRadixSortresultsPass");

  // Verify results
  renderGraph->bufferRead(
      debugBuffer,
      0,
      count * sizeof(uint32_t),
      [count](const void *data)
      {
        uint32_t *sortedKeys = (uint32_t *)data;
        for (uint32_t i = 0; i < count - 1; i++)
        {
          assert(sortedKeys[i + 1] > sortedKeys[i]);
        }
        os::Logger::logf("Sort verification passed for %u elements\n", count);
      });

  delete[] inputKeys;
  delete[] inputValues;

  os::Logger::shutdown();
  return 0;
}