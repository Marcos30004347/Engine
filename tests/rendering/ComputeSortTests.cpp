#include <iostream>

#include "os/File.hpp"
#include "os/Logger.hpp"

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

  uint32_t count = 1024 * 1024;

  uint32_t *data = new uint32_t[count];

  uint32_t maxValue = count;

  for (uint32_t i = 0; i < count; i++)
  {
    data[i] = count - i;
  }

  uint32_t padded_size = gpgpu::RadixSortPass<uint32_t, uint32_t>::keysBufferSize(count);

  Buffer keys = renderGraph->createBuffer(
      BufferInfo{
        .name = "Keys.buffer",
        .size = padded_size * sizeof(uint32_t),
        .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
      });

  Buffer payload = renderGraph->createBuffer(
      BufferInfo{
        .name = "Payload.buffer",
        .size = padded_size * sizeof(uint32_t),
        .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
      });

  Buffer debug = renderGraph->createBuffer(
      BufferInfo{
        .name = "Debug.buffer",
        .size = padded_size * sizeof(uint32_t),
        .usage = BufferUsage::BufferUsage_CopyDst | BufferUsage::BufferUsage_Pull,
      });

  renderGraph->bufferWrite(keys, 0, count * sizeof(uint32_t), (void **)data);
  renderGraph->bufferWrite(payload, 0, count * sizeof(uint32_t), (void **)data);

  RenderGraph::Frame frame;
  RenderGraph::Overrides overrides;

  renderGraph->registerPass<gpgpu::RadixSortPass<uint32_t, uint32_t>>("radixSortPass", 0, count, keys, payload);
  renderGraph->registerPass<gpgpu::CopyBufferPass>("copyRadixSortresultsPass", 1, keys, 0, count * sizeof(uint32_t), debug, 0, count * sizeof(uint32_t));

  renderGraph->compile();
  renderGraph->run(frame, overrides);
  renderGraph->waitFrame(frame);

  renderGraph->removePass<gpgpu::RadixSortPass<uint32_t, uint32_t>>("radixSortPass");
  renderGraph->removePass<gpgpu::CopyBufferPass>("copyRadixSortresultsPass");

  renderGraph->bufferRead(
      debug,
      0,
      count * sizeof(uint32_t),
      [count](const void *data)
      {
        uint32_t *values = (uint32_t *)data;

        for (uint32_t i = 0; i < count - 1; i++)
        {
          assert(values[i + 1] > values[i]);
        }
#ifdef LOG_RESULTS
        std::ostringstream oss;

        for (uint32_t i = 0; i < count; i++)
        {
          oss << values[i];
          oss << ", ";
        }

        std::string s = oss.str();
        os::Logger::logf("%s", s.c_str());
#endif
      });

  os::Logger::shutdown();

  return 0;
}