#include "os/File.hpp"
#include "rhi/Device.hpp"
#include "rhi/vulkan/VulkanDevice.hpp"
#include "time/TimeSpan.hpp"
#include "window/sdl3/SDL3Window.hpp"
#include "window/window.hpp"

int main()
{
  window::Window *window = new window::sdl3::SDL3Window(window::WindowSurface_Vulkan, "Triangle", 800, 800);

  rhi::DeviceRequiredLimits limits = (rhi::DeviceRequiredLimits){
    .minimumMemory = 0,
    .minimumComputeSharedMemory = 0,
    .minimumComputeWorkGroupInvocations = 0,
  };

  rhi::DeviceFeatures features = rhi::DeviceFeatures_None;

  rhi::Device *device = new rhi::vulkan::VulkanDevice(rhi::vulkan::Vulkan_1_2, limits, features, window->getVulkanExtensions());
  rhi::vulkan::VulkanDevice *vkDevice = reinterpret_cast<rhi::vulkan::VulkanDevice *>(device);
  VkSurfaceKHR vkSurface = window->getVulkanSurface(vkDevice->getInstance());

  rhi::Surface surface = vkDevice->addSurface(vkSurface);
  device->init();

  std::string vertexSrc = os::io::readRelativeFile("assets/triangle/shaders/vertex.spv");
  std::string fragmentSrc = os::io::readRelativeFile("assets/triangle/shaders/fragment.spv");

  rhi::Shader vertexShader = vkDevice->createShader({
    .src = vertexSrc,
  });

  rhi::Shader fragmentShader = vkDevice->createShader({
    .src = fragmentSrc,
  });

  rhi::SwapChain swapChain = device->createSwapChain(surface, window->getWidth(), window->getHeight());
  rhi::ColorAttatchment colorAttatchment{};

  colorAttatchment.format = device->getSwapChainFormat(swapChain);
  colorAttatchment.loadOp = rhi::LoadOp::LoadOp_Clear;
  colorAttatchment.storeOp = rhi::StoreOp::StoreOp_Store;

  rhi::GraphicsPipelineInfo graphicsInfo = {};
  graphicsInfo.vertexStage.cullType = rhi::PrimitiveCullType_None;
  graphicsInfo.vertexStage.primitiveType = rhi::PrimitiveType_Triangles;
  graphicsInfo.vertexStage.vertexLayoutElementsCount = 2;

  rhi::VertexLayoutElement vertexElements[2] = {
    // Position
    {
      .binding = 0,
      .location = 0,
      .type = rhi::Type_Float32x3,
      .offset = 0,
    },
    // Color
    {
      .binding = 0,
      .location = 1,
      .type = rhi::Type_Float32x4,
      .offset = sizeof(float) * 3,
    },
  };

  rhi::BindingsLayoutInfo pipelineLayourInfo;
  pipelineLayourInfo.groupsCount = 0;
  rhi::BindingsLayout pipelineLayout = device->createBindingsLayout(pipelineLayourInfo);
  graphicsInfo.layout = pipelineLayout;
  graphicsInfo.vertexStage.vertexLayoutElements = vertexElements;
  graphicsInfo.vertexStage.vertexShader = vertexShader;
  graphicsInfo.vertexStage.shaderEntry = "main";
  graphicsInfo.fragmentStage.fragmentShader = fragmentShader;
  graphicsInfo.fragmentStage.shaderEntry = "main";
  graphicsInfo.fragmentStage.colorAttatchments = &colorAttatchment;
  graphicsInfo.fragmentStage.colorAttatchmentsCount = 1;
  graphicsInfo.fragmentStage.depthAttatchment.loadOp = rhi::LoadOp::LoadOp_Clear;
  graphicsInfo.fragmentStage.depthAttatchment.storeOp = rhi::StoreOp::StoreOp_Store;
  graphicsInfo.fragmentStage.depthAttatchment.format = rhi::Format_Depth32Float;

  rhi::GraphicsPipeline graphicsPipeline = device->createGraphicsPipeline(graphicsInfo);

  rhi::QueueHandle graphicsQueue = device->getQueue(rhi::QueueType::Queue_Graphics);

  rhi::ImageCreateInfo depthCreateInfo;
  depthCreateInfo.width = 800;
  depthCreateInfo.height = 800;
  depthCreateInfo.format = rhi::Format::Format_Depth32Float;
  depthCreateInfo.memoryProperties = rhi::BufferUsage::BufferUsage_None;
  depthCreateInfo.usage = (rhi::ImageUsage)(rhi::ImageUsage::ImageUsage_DepthStencilAttachment | rhi::ImageUsage::ImageUsage_Sampled);

  rhi::Texture depthTexture = device->createImage(depthCreateInfo);
  rhi::TextureView depthTexureView = device->createImageView(depthTexture, rhi::ImageAspectFlags::Depth);

  rhi::BufferUsage verticeHeapUsage = (rhi::BufferUsage)(rhi::BufferUsage::BufferUsage_Vertex | rhi::BufferUsage::BufferUsage_Push);
  rhi::BufferUsage indexHeapUsage = (rhi::BufferUsage)(rhi::BufferUsage::BufferUsage_Index | rhi::BufferUsage::BufferUsage_Push);

  rhi::GPUHeap *verticeHeap = device->allocateHeap(1024, verticeHeapUsage, NULL);
  rhi::GPUHeap *indexHeap = device->allocateHeap(1024, verticeHeapUsage, NULL);

  float verticeData[] = {
    0.0f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.5f, 0.5f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, -0.5f, 0.5f, 0.0f, 0.0, 0.0f, 1.0f, 1.0f,
  };
  // uint32_t indexData[] = {
  //   0,
  //   1,
  //   2,
  // };

  rhi::GPUBuffer verticeBuffer;
  // rhi::GPUBuffer indexBuffer;

  if (verticeHeap->allocate(sizeof(verticeData), verticeBuffer) != rhi::GPUHeap::GPUHeapStatus::OK)
  {
    throw std::runtime_error("Could not allocate vertex buffer");
  }
  // if (indexHeap->allocate(sizeof(uint32_t) * 3, indexBuffer) != rhi::GPUHeap::GPUHeapStatus::OK)
  // {
  //   throw std::runtime_error("Could not allocate index buffer");
  // }

  void *verticePtr = NULL;
  // void *indexPtr = NULL;

  if (device->mapBuffer(verticeBuffer, rhi::BufferMap::BufferMap_Write, &verticePtr) != rhi::BufferMapStatus::BufferMapStatus_Success)
  {
    throw std::runtime_error("Could not map vertex buffer");
  }
  // if (device->mapBuffer(indexBuffer, rhi::BufferMap::BufferMap_Write, &indexPtr) != rhi::BufferMapStatus::BufferMapStatus_Success)
  // {
  //   throw std::runtime_error("Could not map vertex buffer");
  // }

  float *verticeWritePtr = (float *)verticePtr;
  // uint32_t *indexWritePtr = (uint32_t *)indexPtr;

  for (int i = 0; i < sizeof(verticeData) / sizeof(float); i++)
  {
    verticeWritePtr[i] = verticeData[i];
  }
  // for (int i = 0; i < sizeof(indexData) / sizeof(uint32_t); i++)
  // {
  //   indexWritePtr[i] = indexData[i];
  // }

  device->unmapBuffer(verticeBuffer);
  // device->unmapBuffer(indexBuffer);

  uint32_t frame = 0;

  while (!window->shouldClose())
  {
    lib::time::TimeSpan start = lib::time::TimeSpan::now();

    rhi::ColorAttachmentInfo attatchment;
    attatchment.view = device->getCurrentSwapChainTextureView(swapChain);
    attatchment.clearValue = rhi::Color::rgb(0, 0, 0, 1);

    frame += 1;
    frame %= 256;

    rhi::DepthStencilAttachmentInfo depthAttatchment;
    depthAttatchment.clearDepth = 0;
    depthAttatchment.clearStencil = 0;
    depthAttatchment.view = depthTexureView;

    rhi::RenderPassInfo renderPass;
    renderPass.scissor = rhi::Rect2D(0, 0, 800, 800);
    renderPass.viewport = rhi::Viewport(800, 800);
    renderPass.colorAttachmentsCount = 1;
    renderPass.colorAttachments = &attatchment;
    renderPass.depthStencilAttachment = &depthAttatchment;

    rhi::CommandBuffer commandBuffer = device->createCommandBuffer();
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

  device->waitIdle();
  device->tick();

  device->destroyImageView(depthTexureView);
  device->destroyImage(depthTexture);
  device->destroyGraphicsPipeline(graphicsPipeline);
  device->destroyBindingsLayout(pipelineLayout);
  device->destroySwapChain(swapChain);
  device->destroyShader(vertexShader);
  device->destroyShader(fragmentShader);

  device->freeHeap(verticeHeap);
  device->freeHeap(indexHeap);

  delete device;
  delete window;

  return 0;
}