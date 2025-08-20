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
  rhi::vulkan::VulkanDevice *vkDevice = reinterpret_cast<rhi::vulkan::VulkanDevice *>(device);

  device->init();

  uint32_t values[4096];
  for (int i = 0; i < 4096; i++)
  {
    values[i] = i;
  }

  uint32_t dynamicBlockSize = device->alignedDynamicUniformObjectSize(sizeof(uint32_t));
  uint32_t constants[4096] = {};

  rhi::GPUHeap *storageHeap = device->allocateHeap(4096 * sizeof(uint32_t), (rhi::BufferUsage)(rhi::BufferUsage::BufferUsage_Storage | rhi::BufferUsage::BufferUsage_Push), values);
  rhi::GPUHeap *uniformHeap = device->allocateHeap(5 * dynamicBlockSize, (rhi::BufferUsage)(rhi::BufferUsage::BufferUsage_Uniform | rhi::BufferUsage::BufferUsage_Push), NULL);
  rhi::GPUHeap *pullHeap = device->allocateHeap(4096 * sizeof(uint32_t), (rhi::BufferUsage)(rhi::BufferUsage::BufferUsage_Storage | rhi::BufferUsage::BufferUsage_Pull), NULL);

  std::string computeShader = os::io::readRelativeFile("assets/computeAdd/shaders/addCompute.spv");
  rhi::Shader shader = vkDevice->createShader({
    .src = computeShader,
  });

  rhi::BindingGroupLayoutBufferEntry entries[2];
  
  entries[0].binding = 0;
  entries[0].isDynamic = false;
  entries[0].usage = rhi::BufferUsage::BufferUsage_Storage;
  entries[0].visibility = rhi::BindingVisibility::BindingVisibility_Compute;

  entries[1].binding = 1;
  entries[1].isDynamic = true;
  entries[1].usage = rhi::BufferUsage::BufferUsage_Uniform;
  entries[1].visibility = rhi::BindingVisibility::BindingVisibility_Compute;

  rhi::BindingGroupLayout layoutGroup{};
  layoutGroup.buffers = entries;
  layoutGroup.buffersCount = 2;

  rhi::BindingsLayoutInfo layoutInfo;
  layoutInfo.groups = &layoutGroup;
  layoutInfo.groupsCount = 1;

  rhi::GPUBuffer pullBuffer;
  rhi::GPUBuffer storageBuffer;
  rhi::GPUBuffer uniformBuffer;

  if (storageHeap->allocate(4096 * sizeof(uint32_t), storageBuffer) != rhi::GPUHeap::OK)
  {
    throw std::runtime_error("could not allocate storage buffer");
  }
  if (pullHeap->allocate(4096 * sizeof(uint32_t), pullBuffer) != rhi::GPUHeap::OK)
  {
    throw std::runtime_error("could not allocate storage buffer");
  }
  if (uniformHeap->allocate(5 * dynamicBlockSize, uniformBuffer) != rhi::GPUHeap::OK)
  {
    throw std::runtime_error("could not allocate storage buffer");
  }

  uint8_t *uniformData = NULL;
  device->mapBuffer(uniformBuffer, rhi::BufferMap::BufferMap_Write, (void **)&uniformData);

  uint32_t dynamicOffsets[1] = {0};

  for (int i = 0; i < 5; i++)
  {
    uint32_t *x = (uint32_t *)&uniformData[dynamicBlockSize * i];
    *x = i + 2;
  }

  device->unmapBuffer(uniformBuffer);

  rhi::CommandBuffer commandBuffer = device->createCommandBuffer();

  rhi::BindingsLayout layout = device->createBindingsLayout(layoutInfo);
  rhi::BindingBufferInfo buffers[2];
  buffers[0].binding = 0;
  buffers[0].buffer = storageBuffer;
  buffers[1].binding = 1;
  buffers[1].buffer = uniformBuffer;

  rhi::BindingGroupInfo group{};
  group.buffers = buffers;
  group.buffersCount = 2;

  rhi::BindingGroupsInfo groups{};
  groups.groups = &group;
  groups.groupsCount = 1;

  rhi::BindingGroups bindings = device->createBindingGroups(layout, groups);

  rhi::ComputePipelineInfo pipelineInfo{};
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
    if(data[i] != i + 2) {
      throw std::runtime_error("wrong value");
    }
  }

  device->unmapBuffer(pullBuffer);

  pullHeap->free(pullBuffer);
  storageHeap->free(storageBuffer);
  uniformHeap->free(uniformBuffer);

  device->freeHeap(storageHeap);
  device->freeHeap(uniformHeap);
  device->freeHeap(pullHeap);

  device->destroyComputePipeline(pipeline);
  device->destroyBindingGroups(bindings);
  device->destroyBindingsLayout(layout);
  device->destroyShader(shader);

  delete device;
  
  return 0;
}