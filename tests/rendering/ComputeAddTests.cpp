#include <iostream>

#include "os/File.hpp"
#include "os/Logger.hpp"

#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"

using namespace rendering;
using namespace backend;

struct UniformBuffer
{
  uint32_t value;
};

int main()
{
  os::Logger::start();

  DeviceRequiredLimits limits = (DeviceRequiredLimits){
    .minimumMemory = 0,
    .minimumComputeSharedMemory = 0,
    .minimumComputeWorkGroupInvocations = 0,
  };

  DeviceFeatures features = DeviceFeatures::DeviceFeatures_Compute;

  vulkan::VulkanRHI *rhi = new vulkan::VulkanRHI(vulkan::Vulkan_1_2, limits, features, {});
  auto surfaces = std::vector<VkSurfaceKHR>();
  rhi->init(surfaces);

  std::string addShaderSrc = os::io::readRelativeFile("assets/shaders/spirv/addCompute.spv");

  RenderGraph *renderGraph = new RenderGraph(rhi);

  uint32_t count = 1024;

  uint32_t data[count];

  for (uint32_t i = 0; i < count; i++)
  {
    data[i] = i;
  }

  Buffer valuesBuffer = renderGraph->createBuffer(
      BufferInfo{
        .name = "ValuesToAdd.buffer",
        .size = count * sizeof(uint32_t),
        .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
      });

  Buffer pullBuffer = renderGraph->createBuffer(
      BufferInfo{
        .name = "ValuesToPull.buffer",
        .size = count * sizeof(uint32_t),
        .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
      });

  Buffer uniformBuffer = renderGraph->createBuffer(
      BufferInfo{
        .name = "UniformBuffer.buffer",
        .size = sizeof(UniformBuffer),
        .usage = BufferUsage::BufferUsage_Uniform | BufferUsage::BufferUsage_Push,
      });

  UniformBuffer uniform;
  uniform.value = 7;

  renderGraph->bufferWrite(valuesBuffer, 0, count * sizeof(uint32_t), (void **)&data);
  renderGraph->bufferWrite(uniformBuffer, 0, sizeof(UniformBuffer), (void **)&uniform);

  auto addShaderLayout = renderGraph->createBindingsLayout(
      BindingsLayoutInfo{
        .name = "addShaderLayout.layout",
        .groups =
            {
              BindingGroupLayout{
                .buffers =
                    {
                      BindingGroupLayoutBufferEntry{
                        .name = "storageBuffer",
                        .binding = 0,
                        .isDynamic = false,
                        .type = BufferBindingType::BufferBindingType_StorageBuffer,
                        .visibility = BindingVisibility::BindingVisibility_Compute,
                      },

                      BindingGroupLayoutBufferEntry{
                        .name = "AddValueCB",
                        .binding = 1,
                        .isDynamic = false,
                        .type = BufferBindingType::BufferBindingType_UniformBuffer,
                        .visibility = BindingVisibility::BindingVisibility_Compute,
                      },
                    }},
            },
      });

  auto addShader = renderGraph->createShader(
      ShaderInfo{
        .name = "addShader.shader",
        .layout = addShaderLayout,
        .src = addShaderSrc,
        .type = ShaderType::SpirV,
      });

  auto addShaderBindingGroup = renderGraph->createBindingGroups(
      BindingGroupsInfo{
        .layout = addShaderLayout,
        .name = "addShaderBindingGroup",
        .groups =
            {
              GroupInfo{
                .name = "Group0",
                .buffers =
                    {
                      BindingBuffer{
                        .binding = 0,
                        .bufferView =
                            {
                              .buffer = valuesBuffer,
                              .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE,
                              .offset = 0,
                              .size = count * sizeof(uint32_t),
                            }},
                      BindingBuffer{
                        .binding = 1,
                        .bufferView =
                            {
                              .buffer = uniformBuffer,
                              .access = AccessPattern::UNIFORM_READ,
                              .offset = 0,
                              .size = sizeof(UniformBuffer),
                            }},
                    },
              },
            },
      });

  auto addPipeline = renderGraph->createComputePipeline(
      ComputePipelineInfo{
        .entry = "main",
        .layout = addShaderLayout,
        .name = "AddComputePipeline",
        .shader = addShader,
      });

  RHICommandBuffer commandBuffer;

  commandBuffer.cmdBindComputePipeline(addPipeline);
  commandBuffer.cmdBindBindingGroups(addShaderBindingGroup, nullptr, 0);
  commandBuffer.cmdDispatch(count / 64, 1, 1);
  commandBuffer.cmdCopyBuffer(
      BufferView{
        .buffer = valuesBuffer,
        .offset = 0,
        .size = sizeof(uint32_t) * count,
        .access = AccessPattern::SHADER_READ,
      },
      BufferView{
        .buffer = pullBuffer,
        .offset = 0,
        .size = sizeof(uint32_t) * count,
        .access = AccessPattern::SHADER_WRITE,
      });

  renderGraph->enqueuePass("AddPass", commandBuffer);
  renderGraph->compile();

  RenderGraph::Frame frame;

  renderGraph->run(frame);
  renderGraph->waitFrame(frame);
  renderGraph->bufferRead(
      pullBuffer,
      0,
      sizeof(uint32_t) * count,
      [&](const void *data)
      {
        uint32_t *values = (uint32_t *)(data);
        for (uint32_t i = 0; i < count; i++)
        {
          assert(i + uniform.value == values[i]);
        }
      });

  renderGraph->deleteComputePipeline(addPipeline);
  renderGraph->deleteBindingGroups(addShaderBindingGroup);
  renderGraph->deleteBindingsLayout(addShaderLayout);
  renderGraph->deleteBuffer(valuesBuffer);
  renderGraph->deleteBuffer(uniformBuffer);
  renderGraph->deleteBuffer(pullBuffer);

  renderGraph->deleteShader(addShader);

  os::Logger::shutdown();
  return 0;
}