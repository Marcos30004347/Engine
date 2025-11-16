#include "os/File.hpp"
#include "rhi/Device.hpp"
#include "rhi/vulkan/VulkanDevice.hpp"
#include "time/TimeSpan.hpp"

int main()
{
  rhi::DeviceRequiredLimits limits = (rhi::DeviceRequiredLimits){
    .minimumMemory = 0,
    .minimumComputeSharedMemory = 0,
    .minimumComputeWorkGroupInvocations = 0,
  };

  rhi::DeviceFeatures features = rhi::DeviceFeatures::DeviceFeatures_Compute;

  rhi::Device *device = new rhi::vulkan::VulkanDevice(rhi::vulkan::Vulkan_1_2, limits, features, {});
  {
    rhi::vulkan::VulkanDevice *vkDevice = reinterpret_cast<rhi::vulkan::VulkanDevice *>(device);

    device->init();

    uint32_t values[4096];
    for (int i = 0; i < 4096; i++)
    {
      values[i] = i;
    }

    uint32_t dynamicBlockSize = device->alignedDynamicUniformObjectSize(sizeof(uint32_t));
    uint32_t constants[4096] = {};

    rhi::Buffer storageHeap = device->createBuffer(
        rhi::BufferInfo{
          .name = "StorageBuffer",
          .size = 4096 * sizeof(uint32_t),
          .usage = (rhi::BufferUsage)(rhi::BufferUsage::BufferUsage_Storage | rhi::BufferUsage::BufferUsage_Push),
        },
        values);
    rhi::Buffer uniformHeap = device->createBuffer(
        rhi::BufferInfo{
          .name = "UniformBuffer",
          .size = 5 * dynamicBlockSize,
          .usage = (rhi::BufferUsage)(rhi::BufferUsage::BufferUsage_Uniform | rhi::BufferUsage::BufferUsage_Push),
        },
        NULL);
    rhi::Buffer pullHeap = device->createBuffer(
        rhi::BufferInfo{
          .name = "PullBuffer",
          .size = 4096 * sizeof(uint32_t),
          .usage = (rhi::BufferUsage)(rhi::BufferUsage::BufferUsage_Storage | rhi::BufferUsage::BufferUsage_Pull),
        },
        NULL);

    rhi::BindingGroupLayoutBufferEntry entries[2];
    entries[0].name = "Buffer";
    entries[0].binding = 0;
    entries[0].isDynamic = false;
    entries[0].usage = rhi::BufferUsage::BufferUsage_Storage;
    entries[0].visibility = rhi::BindingVisibility::BindingVisibility_Compute;

    entries[1].name = "Uniform";
    entries[1].binding = 1;
    entries[1].isDynamic = true;
    entries[1].usage = rhi::BufferUsage::BufferUsage_Uniform;
    entries[1].visibility = rhi::BindingVisibility::BindingVisibility_Compute;

    rhi::BindingGroupLayout layoutGroup{};
    layoutGroup.buffers.push_back(entries[0]);
    layoutGroup.buffers.push_back(entries[1]);

    rhi::BindingsLayoutInfo layoutInfo;
    layoutInfo.name = "LayoutInfo";
    layoutInfo.groups.push_back(layoutGroup);

    std::string computeShader = os::io::readRelativeFile("assets/computeAdd/shaders/addCompute.spv");
    rhi::Shader shader = vkDevice->createShader(
        {
          .src = computeShader,
        },
        layoutInfo);

    rhi::BufferView pullBuffer = {
      .buffer = pullHeap,
      .offset = 0,
      .size = sizeof(uint32_t) * 4096,
    };

    rhi::BufferView storageBuffer = {
      .buffer = storageHeap,
      .offset = 0,
      .size = sizeof(uint32_t) * 4096,
    };

    rhi::BufferView uniformBuffer = {
      .buffer = uniformHeap,
      .offset = 0,
      .size = sizeof(uint32_t) * dynamicBlockSize,
    };

    uint8_t *uniformData = NULL;
    device->mapBuffer(uniformBuffer, rhi::BufferMap::BufferMap_Write, (void **)&uniformData);

    uint32_t dynamicOffsets[1] = {0};

    for (int i = 0; i < 5; i++)
    {
      uint32_t *x = (uint32_t *)&uniformData[dynamicBlockSize * i];
      *x = i + 2;
    }

    device->unmapBuffer(uniformBuffer);

    rhi::CommandBuffer commandBuffer = device->createCommandBuffer(
        rhi::CommandBufferInfo{
          .name = "CommandBuffer",
        });

    rhi::BindingsLayout layout = device->createBindingsLayout(layoutInfo);
    rhi::BindingBufferInfo buffers[2];
    buffers[0].name = "StorageBufferBinding";
    buffers[0].binding = 0;
    buffers[0].buffer = storageBuffer;
    buffers[1].name = "UniformBufferBinding";
    buffers[1].binding = 1;
    buffers[1].buffer = uniformBuffer;

    rhi::BindingGroupInfo group{};
    group.buffers.push_back(buffers[0]);
    group.buffers.push_back(buffers[1]);

    rhi::BindingGroupsInfo groups{};
    groups.groups.push_back(group);
    groups.layout = layout;
    
    rhi::BindingGroups bindings = device->createBindingGroups(groups);

    rhi::ComputePipelineInfo pipelineInfo{};
    pipelineInfo.name = "ComputePipeline";
    pipelineInfo.entry = "main";
    pipelineInfo.shader = shader;
    pipelineInfo.layout = layout;
    rhi::ComputePipeline pipeline = device->createComputePipeline(pipelineInfo);

    device->beginCommandBuffer(commandBuffer);
    device->cmdBindComputePipeline(commandBuffer, pipeline);
    device->cmdBindBindingGroups(commandBuffer, bindings, dynamicOffsets, 1);
    device->cmdDispatch(commandBuffer, 4096 / 64, 1, 1);
    device->cmdCopyBuffer(commandBuffer, storageBuffer, pullBuffer, 0, 0, storageBuffer.size);
    device->endCommandBuffer(commandBuffer);

    rhi::GPUFuture promise = device->submit(device->getQueue(rhi::Queue_Compute), &commandBuffer, 1);

    device->wait(promise);

    uint32_t *data;

    if (device->mapBuffer(pullBuffer, rhi::BufferMap::BufferMap_Read, (void **)&data) != rhi::BufferMapStatus::BufferMapStatus_Success)
    {
      throw std::runtime_error("Could not map buffer");
    }

    for (int i = 0; i < 4096; i++)
    {
      if (data[i] != i + 2)
      {
        throw std::runtime_error("wrong value");
      }
    }

    device->unmapBuffer(pullBuffer);
  }
  // device->destroyComputePipeline(pipeline);
  // device->destroyBindingGroups(bindings);
  // device->destroyBindingsLayout(layout);
  // device->destroyShader(shader);

  delete device;

  return 0;
}