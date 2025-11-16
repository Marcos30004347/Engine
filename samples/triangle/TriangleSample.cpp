#include "os/File.hpp"
#include "rhi/Device.hpp"

#define VULKAN_DEVICE_LOG
#include "rhi/vulkan/VulkanDevice.hpp"

#include "time/TimeSpan.hpp"
#include "window/sdl3/SDL3Window.hpp"
#include "window/window.hpp"

int main()
{
  os::Logger::setIdleSleep(lib::time::TimeSpan::fromMilliseconds(10));
  os::Logger::start();

  window::Window *window = new window::sdl3::SDL3Window(window::WindowSurface_Vulkan, "Triangle", 800, 800);

  rhi::DeviceRequiredLimits limits = (rhi::DeviceRequiredLimits){
    .minimumMemory = 0,
    .minimumComputeSharedMemory = 0,
    .minimumComputeWorkGroupInvocations = 0,
  };

  rhi::DeviceFeatures features = rhi::DeviceFeatures_None;

  rhi::Device *device = new rhi::vulkan::VulkanDevice(rhi::vulkan::Vulkan_1_2, limits, features, window->getVulkanExtensions());
  {
    rhi::vulkan::VulkanDevice *vkDevice = reinterpret_cast<rhi::vulkan::VulkanDevice *>(device);
    VkSurfaceKHR vkSurface = window->getVulkanSurface(vkDevice->getInstance());

    rhi::Surface surface = vkDevice->addSurface(
        vkSurface,
        rhi::SurfaceInfo{
          .name = "Surface",
        });

    device->init();

    std::string vertexSrc = os::io::readRelativeFile("assets/triangle/shaders/vertex.spv");
    std::string fragmentSrc = os::io::readRelativeFile("assets/triangle/shaders/fragment.spv");

    rhi::BindingsLayoutInfo pipelineLayourInfo;
    pipelineLayourInfo.name = "SimpleBindingsLayout";

    rhi::Shader vertexShader = vkDevice->createShader(
        {
          .src = vertexSrc,
        },
        pipelineLayourInfo);

    rhi::Shader fragmentShader = vkDevice->createShader(
        {
          .src = fragmentSrc,
        },
        pipelineLayourInfo);

    rhi::SwapChain swapChain = device->createSwapChain(surface, window->getWidth(), window->getHeight());
    rhi::ColorAttatchment colorAttatchment{};

    colorAttatchment.format = device->getSwapChainFormat(swapChain);
    colorAttatchment.loadOp = rhi::LoadOp::LoadOp_Clear;
    colorAttatchment.storeOp = rhi::StoreOp::StoreOp_Store;

    rhi::GraphicsPipelineInfo graphicsInfo = {};
    graphicsInfo.name = "SimpleGraphicsPipeline";
    graphicsInfo.vertexStage.cullType = rhi::PrimitiveCullType_None;
    graphicsInfo.vertexStage.primitiveType = rhi::PrimitiveType_Triangles;

    rhi::VertexLayoutElement vertexElements[2] = {
      // Position
      {
        .name = "Position",
        .binding = 0,
        .location = 0,
        .type = rhi::Type_Float32x3,
        .offset = 0,
      },
      // Color
      {
        .name = "Color",
        .binding = 0,
        .location = 1,
        .type = rhi::Type_Float32x4,
        .offset = sizeof(float) * 3,
      },
    };

    rhi::BindingsLayout pipelineLayout = device->createBindingsLayout(pipelineLayourInfo);
    graphicsInfo.layout = pipelineLayout;
    graphicsInfo.vertexStage.vertexLayoutElements = {vertexElements[0], vertexElements[1]};
    graphicsInfo.vertexStage.vertexShader = vertexShader;
    graphicsInfo.vertexStage.shaderEntry = "main";
    graphicsInfo.fragmentStage.fragmentShader = fragmentShader;
    graphicsInfo.fragmentStage.shaderEntry = "main";
    graphicsInfo.fragmentStage.colorAttatchments = {colorAttatchment};
    graphicsInfo.fragmentStage.depthAttatchment.loadOp = rhi::LoadOp::LoadOp_Clear;
    graphicsInfo.fragmentStage.depthAttatchment.storeOp = rhi::StoreOp::StoreOp_Store;
    graphicsInfo.fragmentStage.depthAttatchment.format = rhi::Format_Depth32Float;

    rhi::GraphicsPipeline graphicsPipeline = device->createGraphicsPipeline(graphicsInfo);

    rhi::QueueHandle graphicsQueue = device->getQueue(rhi::QueueType::Queue_Graphics);

    rhi::TextureInfo depthCreateInfo;
    depthCreateInfo.name = "DepthTexture";
    depthCreateInfo.width = 800;
    depthCreateInfo.height = 800;
    depthCreateInfo.format = rhi::Format::Format_Depth32Float;
    depthCreateInfo.memoryProperties = rhi::BufferUsage::BufferUsage_None;
    depthCreateInfo.usage = (rhi::ImageUsage)(rhi::ImageUsage::ImageUsage_DepthStencilAttachment | rhi::ImageUsage::ImageUsage_Sampled);
    rhi::Texture depthTexture = device->createTexture(depthCreateInfo);

    rhi::TextureViewInfo depthTextureViewInfo;
    depthTextureViewInfo.name = "DepthTextureView";
    depthTextureViewInfo.flags = rhi::ImageAspectFlags::Depth;
    depthTextureViewInfo.texture = depthTexture;
    rhi::TextureView depthTexureView = device->createTextureView(depthTextureViewInfo);

    rhi::BufferUsage verticeHeapUsage = (rhi::BufferUsage)(rhi::BufferUsage::BufferUsage_Vertex | rhi::BufferUsage::BufferUsage_Push);
    rhi::BufferUsage indexHeapUsage = (rhi::BufferUsage)(rhi::BufferUsage::BufferUsage_Index | rhi::BufferUsage::BufferUsage_Push);

    rhi::Buffer verticeHeap = device->createBuffer(
        rhi::BufferInfo{
          .name = "VerticeBuffer",
          .size = 1024,
          .usage = verticeHeapUsage,
        },
        NULL);

    rhi::Buffer indexHeap = device->createBuffer(
        rhi::BufferInfo{
          .name = "IndexBuffer",
          .size = 1024,
          .usage = verticeHeapUsage,
        },
        NULL);

    float verticeData[] = {
      0.0f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.5f, 0.5f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, -0.5f, 0.5f, 0.0f, 0.0, 0.0f, 1.0f, 1.0f,
    };

    rhi::BufferView verticeBuffer = {
      .buffer = verticeHeap,
      .offset = 0,
      .size = sizeof(verticeData),
    };

    void *verticePtr = NULL;

    if (device->mapBuffer(verticeBuffer, rhi::BufferMap::BufferMap_Write, &verticePtr) != rhi::BufferMapStatus::BufferMapStatus_Success)
    {
      throw std::runtime_error("Could not map vertex buffer");
    }

    float *verticeWritePtr = (float *)verticePtr;

    for (int i = 0; i < sizeof(verticeData) / sizeof(float); i++)
    {
      verticeWritePtr[i] = verticeData[i];
    }

    device->unmapBuffer(verticeBuffer);

    uint32_t frame = 0;

    while (!window->shouldClose())
    {
      lib::time::TimeSpan start = lib::time::TimeSpan::now();

      rhi::ColorAttachmentInfo attatchment;
      attatchment.name = "ColorAttatchment";
      attatchment.view = device->getCurrentSwapChainTextureView(swapChain);
      attatchment.clearValue = rhi::Color::rgb(0, 0, 0, 1);

      frame += 1;
      frame %= 256;

      rhi::DepthStencilAttachmentInfo depthAttatchment;
      depthAttatchment.name = "DepthAttatchment";
      depthAttatchment.clearDepth = 0;
      depthAttatchment.clearStencil = 0;
      depthAttatchment.view = depthTexureView;

      rhi::RenderPassInfo renderPass;
      renderPass.name = "RenderPass";
      renderPass.scissor = rhi::Rect2D(0, 0, 800, 800);
      renderPass.viewport = rhi::Viewport(800, 800);
      renderPass.colorAttachmentsCount = 1;
      renderPass.colorAttachments = &attatchment;
      renderPass.depthStencilAttachment = &depthAttatchment;

      rhi::CommandBuffer commandBuffer = device->createCommandBuffer(
          rhi::CommandBufferInfo{
            .name = "CommandBuffer",
          });

      device->beginCommandBuffer(commandBuffer);
      device->cmdBindGraphicsPipeline(commandBuffer, graphicsPipeline);
      device->cmdBeginRenderPass(commandBuffer, renderPass);
      device->cmdBindVertexBuffer(commandBuffer, 0, verticeBuffer);
      device->cmdDraw(commandBuffer, 3, 1, 0, 0);
      device->cmdEndRenderPass(commandBuffer);
      device->endCommandBuffer(commandBuffer);

      rhi::GPUFuture future = device->submit(graphicsQueue, &commandBuffer, 1);

      device->tick();
      window->update();
    }
  }

  device->waitIdle();
  device->tick();

  delete device;
  delete window;

  os::Logger::shutdown();

  return 0;
}