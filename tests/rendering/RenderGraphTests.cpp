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

  RHI *rhi = new vulkan::VulkanRHI(vulkan::Vulkan_1_2, limits, features, {});

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
              .visibility = BindingVisibility::BindingVisibility_Compute,
            },
            BindingGroupLayoutBufferEntry{
              .binding = 1,
              .isDynamic = false,
              .name = "SecondBuffer",
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
              .binding = 0,
              .isDynamic = false,
              .name = "FirstBuffer",
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
              .visibility = BindingVisibility::BindingVisibility_Compute,
            },
          },
    },
  };

  // renderGraph->importBuffer(renderGraphB, "BufferA", "RenderPassB_Buffera");
  auto bufferAInfo = mockBufferInfo("BufferA");
  auto bufferBInfo = mockBufferInfo("BufferB");
  auto textureAInfo = mockTextureInfo("TextureA");
  auto samplerAInfo = mockSamplerInfo("SamplerA");
  auto bufferCInfo = mockBufferInfo("BufferC");

  auto buffer = renderGraph->createBuffer(bufferAInfo);
  auto bufferB = renderGraph->createBuffer(bufferBInfo);
  auto bufferC = renderGraph->createBuffer(bufferCInfo);
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
                },
            .samplers =
                {
                  {
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
                          .layout = ResourceLayout::COLOR_ATTACHMENT,
                        },
                  },
                },
          },
        },
  };
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
                      .offset = 512,
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
                      .offset = 512 + 256,
                      .size = 256,
                      .access = AccessPattern::SHADER_READ,
                    },
              },
            },
      },
    }};

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

  renderGraph->createBindingGroups(bindingGroupsBInfo);
  renderGraph->createBindingGroups(bindingGroupsCInfo);
  renderGraph->createBindingGroups(bindingGroupsDInfo);
  renderGraph->createBindingGroups(bindingGroupsEInfo);
  renderGraph->createBindingGroups(bindingGroupsFInfo);

  renderGraph->addPass(
      "passB",
      RenderGraph::ExecuteAlways,
      [](ResourceDatabase &resources, CommandRecorder &cmd)
      {
        Texture textureA = resources.getTexture("TextureA");
        Buffer buffer = resources.getBuffer("BufferA");
        Sampler sampler = resources.getSampler("SamplerA");
        BindingGroups bindings = resources.getBindingGroups("BindingsPassB");

        cmd.cmdBindBindingGroups(bindings, nullptr, 0);
        cmd.cmdDispatch(0, 0, 0);
      });

  renderGraph->addPass(
      "passC",
      RenderGraph::ExecuteAlways,
      [](ResourceDatabase &resources, CommandRecorder &cmd)
      {
        Buffer buffer = resources.getBuffer("BufferA");
        Buffer bufferC = resources.getBuffer("BufferC");
        BindingGroups bindings = resources.getBindingGroups("BindingsPassC");

        cmd.cmdBindBindingGroups(bindings, nullptr, 0);
        cmd.cmdDispatch(0, 0, 0);
      });

  renderGraph->addPass(
      "passD",
      RenderGraph::ExecuteAlways,
      [](ResourceDatabase &resources, CommandRecorder &cmd)
      {
        BindingGroups bindings = resources.getBindingGroups("BindingsPassD");
        Buffer buffer = resources.getBuffer("BufferA");

        cmd.cmdBindBindingGroups(bindings, nullptr, 0);
        cmd.cmdDispatch(0, 0, 0);
      });

  renderGraph->addPass(
      "passE",
      RenderGraph::ExecuteAlways,
      [](ResourceDatabase &resources, CommandRecorder &cmd)
      {
        Buffer buffer = resources.getBuffer("BufferA");
        BindingGroups bindings = resources.getBindingGroups("BindingsPassE");

        cmd.cmdBindBindingGroups(bindings, nullptr, 0);
        cmd.cmdDispatch(0, 0, 0);
      });

  renderGraph->addPass(
      "passF",
      RenderGraph::ExecuteAlways,
      [](ResourceDatabase &resources, CommandRecorder &cmd)
      {
        Buffer buffer = resources.getBuffer("BufferA");
        BindingGroups bindings = resources.getBindingGroups("BindingsPassF");
        cmd.cmdBindBindingGroups(bindings, nullptr, 0);
        cmd.cmdDispatch(0, 0, 0);
      });

  lib::time::TimeSpan now = lib::time::TimeSpan::now();
  renderGraph->compile();
  lib::time::TimeSpan then = lib::time::TimeSpan::now();

  // const Buffer &buffer = renderGraph->getBuffer("BufferA");

  // const uint64_t data[] = {0,1,2,3,4,5};
  // rhi->bufferWrite(buffer, 0, sizeof(data), (void*)data);

  // GPUFuture future = renderGraph->execute();

  os::Logger::logf("Task Graph compilation time = %fms", (then - now).milliseconds());

  // program.log();

  os::Logger::shutdown();

  return 0;
}
