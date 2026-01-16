#include "os/Logger.hpp"
#include "rendering/gpu/RHI.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "time/TimeSpan.hpp"

using namespace rendering;
using namespace backend;

BufferInfo mockBufferInfo(std::string name)
{
  return BufferInfo{
    .name = name,
    .size = 1024,
    .usage = BufferUsage::BufferUsage_Storage,
  };
}

TextureInfo mockTextureInfo(std::string name)
{
  return TextureInfo{
    .name = name,
    .width = 1024,
    .height = 1024,
    .memoryProperties = BufferUsage::BufferUsage_Storage,
    .usage = ImageUsage::ImageUsage_Sampled,
    .mipLevels = 4,
    .depth = 4,
    .format = Format::Format_RGBA8Uint,
  };
}

SamplerInfo mockSamplerInfo(std::string name)
{
  return SamplerInfo{
    .addressModeU = SamplerAddressMode::Repeat,
    .addressModeV = SamplerAddressMode::Repeat,
    .addressModeW = SamplerAddressMode::Repeat,
    .anisotropyEnable = true,
    .magFilter = Filter::Linear,
    .maxAnisotropy = 1.0f,
    .maxLod = 1.0f,
    .minFilter = Filter::Linear,
    .name = name,
  };
}

int main()
{
  os::Logger::start();

  DeviceRequiredLimits limits = (DeviceRequiredLimits){
    .minimumMemory = 0,
    .minimumComputeSharedMemory = 0,
    .minimumComputeWorkGroupInvocations = 0,
  };

  DeviceFeatures features = DeviceFeatures::DeviceFeatures_Compute;

  // Device *device = new vulkan::VulkanDevice(vulkan::Vulkan_1_2, limits, features, {});

  // device->init();
  // RHI *rhi = new VulkanRHI();

  vulkan::VulkanRHI *rhi = new vulkan::VulkanRHI(vulkan::Vulkan_1_2, limits, features, {});
  auto surfaces = std::vector<VkSurfaceKHR>();
  rhi->init(surfaces);

  RenderGraph *renderGraph = new RenderGraph(rhi);

  BindingsLayoutInfo layoutBInfo;
  layoutBInfo.name = "BindingLayoutB";
  layoutBInfo.groups = {
    BindingGroupLayout{
      .buffers =
          {
            BindingGroupLayoutBufferEntry{
              .binding = 0,
              .isDynamic = false,
              .name = "FirstBuffer",
              .type = BufferBindingType::BufferBindingType_StorageBuffer,
              .visibility = BindingVisibility::BindingVisibility_Compute,
            },
          },
      .samplers =
          {
            BindingGroupLayoutSamplerEntry{
              .binding = 1,
              .visibility = BindingVisibility::BindingVisibility_Compute,
            },
          }},
  };
  BindingsLayoutInfo layoutCInfo;
  layoutCInfo.name = "BindingLayoutC";
  layoutCInfo.groups = {
    BindingGroupLayout{
      .buffers =
          {
            BindingGroupLayoutBufferEntry{
              .binding = 0,
              .isDynamic = false,
              .name = "FirstBuffer",
              .type = BufferBindingType::BufferBindingType_StorageBuffer,
              .visibility = BindingVisibility::BindingVisibility_Compute,
            },
            BindingGroupLayoutBufferEntry{
              .binding = 1,
              .isDynamic = false,
              .name = "SecondBuffer",
              .type = BufferBindingType::BufferBindingType_StorageBuffer,
              .visibility = BindingVisibility::BindingVisibility_Compute,
            },
          },
    },
  };
  BindingsLayoutInfo layoutDInfo;
  layoutDInfo.name = "BindingLayoutD";
  layoutDInfo.groups = {
    BindingGroupLayout{
      .buffers =
          {
            BindingGroupLayoutBufferEntry{
              .name = "FirstBuffer",

              .binding = 0,
              .isDynamic = false,
              .type = BufferBindingType::BufferBindingType_StorageBuffer,
              .visibility = BindingVisibility::BindingVisibility_Compute,
            },
          },
    },
  };

  BindingsLayoutInfo layoutEInfo;
  layoutEInfo.name = "BindingLayoutE";
  layoutEInfo.groups = {
    BindingGroupLayout{
      .buffers =
          {
            BindingGroupLayoutBufferEntry{
              .binding = 0,
              .isDynamic = false,
              .name = "FirstBuffer",
              .type = BufferBindingType::BufferBindingType_StorageBuffer,
              .visibility = BindingVisibility::BindingVisibility_Compute,
            },
          },
    },
  };
  BindingsLayoutInfo layoutFInfo;
  layoutFInfo.name = "BindingLayoutF";
  layoutFInfo.groups = {
    BindingGroupLayout{
      .buffers =
          {
            BindingGroupLayoutBufferEntry{
              .binding = 0,
              .isDynamic = false,
              .name = "FirstBuffer",
              .type = BufferBindingType::BufferBindingType_StorageBuffer,
              .visibility = BindingVisibility::BindingVisibility_Compute,
            },
          },
    },
  };

  // renderGraph->importBuffer(renderGraphB, "BufferA", "RenderPassB_Buffera");
  auto bufferAInfo = mockBufferInfo("BufferA");
  auto bufferBInfo = mockBufferInfo("BufferB");
  auto bufferCInfo = mockBufferInfo("BufferC");
  auto bufferDInfo = mockBufferInfo("BufferD");

  auto textureAInfo = mockTextureInfo("TextureA");
  auto samplerAInfo = mockSamplerInfo("SamplerA");

  auto buffer = renderGraph->createBuffer(bufferAInfo);
  auto bufferB = renderGraph->createBuffer(bufferBInfo);
  auto bufferC = renderGraph->createBuffer(bufferCInfo);
  auto bufferD = renderGraph->createBuffer(bufferDInfo);

  auto textureA = renderGraph->createTexture(textureAInfo);
  auto samplerA = renderGraph->createSampler(samplerAInfo);

  auto layoutB = renderGraph->createBindingsLayout(layoutBInfo);
  auto layoutC = renderGraph->createBindingsLayout(layoutCInfo);
  auto layoutD = renderGraph->createBindingsLayout(layoutDInfo);
  auto layoutE = renderGraph->createBindingsLayout(layoutEInfo);
  auto layoutF = renderGraph->createBindingsLayout(layoutFInfo);

  auto bindingGroupsBInfo = BindingGroupsInfo{
    .name = "BindingsPassB",
    .layout = layoutB,
    .groups =
        {
          GroupInfo{
            .buffers =
                {
                  {
                    .binding = 0,
                    .bufferView =
                        {
                          .buffer = buffer,
                          .offset = 0,
                          .size = 512,
                          .access = AccessPattern::MEMORY_WRITE,
                        },
                  },
                },
            .samplers =
                {
                  BindingSampler{
                    .binding = 1,
                    .sampler = samplerA,
                    .view =
                        {
                          .texture = textureA,
                          .access = AccessPattern::COLOR_ATTACHMENT_READ,
                          .baseArrayLayer = 0,
                          .baseMipLevel = 0,
                          .layerCount = 1,
                          .levelCount = 1,
                          .flags = ImageAspectFlags::Color,
                          .layout = ResourceLayout::SHADER_READ_ONLY,
                        },
                  },
                },
          },
        },
  };

  renderGraph->createBindingGroups(bindingGroupsBInfo);
  auto bindingGroupsCInfo = BindingGroupsInfo{
    .name = "BindingsPassC",
    .layout = layoutC,
    .groups = {
      {
        .buffers =
            {
              {
                .binding = 0,
                .bufferView =
                    {
                      .buffer = buffer,
                      .offset = 0,
                      .size = 512,
                      .access = AccessPattern::COLOR_ATTACHMENT_WRITE,
                    },
              },
              {
                .binding = 1,
                .bufferView =
                    {
                      .buffer = bufferC,
                      .offset = 0,
                      .size = 1024,
                      .access = AccessPattern::COLOR_ATTACHMENT_WRITE,
                    },
              },
            },
      },
    }};

  renderGraph->createBindingGroups(bindingGroupsCInfo);

  auto bindingGroupsDInfo = BindingGroupsInfo{
    .name = "BindingsPassD",
    .layout = layoutD,
    .groups = {
      {
        .buffers =
            {
              BindingBuffer{
                .binding = 0,
                .bufferView =
                    {
                      .buffer = buffer,
                      .offset = 0,
                      .size = 512,
                      .access = AccessPattern::SHADER_READ,
                    },
              },
            },
      },
    }};
  renderGraph->createBindingGroups(bindingGroupsDInfo);

  auto bindingGroupsEInfo = BindingGroupsInfo{
    .name = "BindingsPassE",
    .layout = layoutE,
    .groups = {
      {
        .buffers =
            {
              {
                .binding = 0,
                .bufferView =
                    {
                      .buffer = buffer,
                      .offset = 0,
                      .size = 256,
                      .access = AccessPattern::SHADER_READ,
                    },
              },
            },
      },
    }};
  
  renderGraph->createBindingGroups(bindingGroupsEInfo);

  auto bindingGroupsFInfo = BindingGroupsInfo{
    .name = "BindingsPassF",
    .layout = layoutF,
    .groups = {
      {
        .buffers =
            {
              {
                .binding = 0,
                .bufferView =
                    {
                      .buffer = buffer,
                      .offset = 0,
                      .size = 1024,
                      .access = AccessPattern::SHADER_WRITE,
                    },
              },
            },
      },
    }};

  renderGraph->createBindingGroups(bindingGroupsFInfo);

  auto bindingsB = renderGraph->getBindingGroups("BindingsPassB");
  auto bindingsC = renderGraph->getBindingGroups("BindingsPassC");
  auto bindingsD = renderGraph->getBindingGroups("BindingsPassD");
  auto bindingsE = renderGraph->getBindingGroups("BindingsPassE");
  auto bindingsF = renderGraph->getBindingGroups("BindingsPassF");

  RHICommandBuffer passB;
  passB.cmdBindBindingGroups(bindingsB, nullptr, 0);
  passB.cmdDispatch(0, 0, 0);

  RHICommandBuffer passC;
  passC.cmdBindBindingGroups(bindingsC, nullptr, 0);
  passC.cmdDispatch(0, 0, 0);

  RHICommandBuffer passD;
  passD.cmdBindBindingGroups(bindingsD, nullptr, 0);
  passD.cmdDispatch(0, 0, 0);

  RHICommandBuffer passE;
  passE.cmdBindBindingGroups(bindingsE, nullptr, 0);
  passE.cmdDispatch(0, 0, 0);

  RHICommandBuffer passF;
  passF.cmdBindBindingGroups(bindingsF, nullptr, 0);
  passF.cmdDispatch(0, 0, 0);

  renderGraph->enqueuePass("passB", passB);
  renderGraph->enqueuePass("passC", passC);
  renderGraph->enqueuePass("passD", passD);
  renderGraph->enqueuePass("passE", passE);
  renderGraph->enqueuePass("passF", passF);

  lib::time::TimeSpan now = lib::time::TimeSpan::now();
  renderGraph->compile();
  lib::time::TimeSpan then = lib::time::TimeSpan::now();

  os::Logger::logf("Task Graph compilation time = %fms", (then - now).milliseconds());

  renderGraph->deleteBindingGroups(bindingsB);
  renderGraph->deleteBindingGroups(bindingsC);
  renderGraph->deleteBindingGroups(bindingsD);
  renderGraph->deleteBindingGroups(bindingsE);
  renderGraph->deleteBindingGroups(bindingsF);
  renderGraph->deleteBindingsLayout(layoutB);
  renderGraph->deleteBindingsLayout(layoutC);
  renderGraph->deleteBindingsLayout(layoutD);
  renderGraph->deleteBindingsLayout(layoutE);
  renderGraph->deleteBindingsLayout(layoutF);

  renderGraph->deleteBuffer(buffer);
  renderGraph->deleteBuffer(bufferB);
  renderGraph->deleteBuffer(bufferC);
  renderGraph->deleteBuffer(bufferD);
  renderGraph->deleteSampler(samplerA);

  os::Logger::shutdown();

  return 0;
}
