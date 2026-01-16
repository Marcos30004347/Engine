#include <iostream>

#include "os/File.hpp"
#include "os/Logger.hpp"

#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "time/TimeSpan.hpp"
#include "window/sdl3/SDL3Window.hpp"
#include "window/window.hpp"
using namespace rendering;
using namespace backend;

int main()
{
  os::Logger::start(100);
  window::Window *window = new window::sdl3::SDL3Window(window::WindowSurface_Vulkan, "Triangle", 800, 800);

  DeviceRequiredLimits limits = (DeviceRequiredLimits){
    .minimumMemory = 0,
    .minimumComputeSharedMemory = 0,
    .minimumComputeWorkGroupInvocations = 0,
  };

  DeviceFeatures features = DeviceFeatures::DeviceFeatures_Compute;

  vulkan::VulkanRHI *rhi = new vulkan::VulkanRHI(vulkan::Vulkan_1_2, limits, features, {});

  auto surfaces = std::vector<VkSurfaceKHR>();

  surfaces.push_back(window->getVulkanSurface(rhi->getInstance()));

  rhi->init(surfaces);

  std::string vertexShaderSrc = os::io::readRelativeFile("assets/shaders/spirv/vertex.spv");
  std::string fragmentShaderSrc = os::io::readRelativeFile("assets/shaders/spirv/fragment.spv");

  RenderGraph *renderGraph = new RenderGraph(rhi);

  float verticeData[] = {
    0.0f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.5f, 0.5f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, -0.5f, 0.5f, 0.0f, 0.0, 0.0f, 1.0f, 1.0f,
  };

  Buffer vertexBuffer = renderGraph->createBuffer(
      BufferInfo{
        .name = "Vertice.buffer",
        .size = sizeof(verticeData),
        .usage = BufferUsage::BufferUsage_Vertex | BufferUsage::BufferUsage_Push,
      });

  renderGraph->bufferWrite(vertexBuffer, 0, sizeof(verticeData), (void **)&verticeData);

  auto pipelineLayout = renderGraph->createBindingsLayout({
    .name = "PipelineLayout",
  });

  auto bindingGroups = renderGraph->createBindingGroups({
    .layout = pipelineLayout,
    .name = "BindingGroups",
  });

  auto vertexShader = renderGraph->createShader(
      ShaderInfo{
        .name = "vertexShader.shader",
        .layout = pipelineLayout,
        .src = vertexShaderSrc,
        .type = ShaderType::SpirV,
      });

  auto fragmentShader = renderGraph->createShader(
      ShaderInfo{
        .name = "fragmentShader.shader",
        .layout = pipelineLayout,
        .src = fragmentShaderSrc,
        .type = ShaderType::SpirV,
      });

  SwapChain swapChain = rhi->createSwapChain(0, window->getWidth(), window->getHeight());

  renderGraph->addSwapChainImages(swapChain);

  ColorAttatchment colorAttatchment{};

  colorAttatchment.format = rhi->getSwapChainFormat(swapChain);
  colorAttatchment.loadOp = LoadOp::LoadOp_Clear;
  colorAttatchment.storeOp = StoreOp::StoreOp_Store;

  GraphicsPipelineInfo graphicsInfo = {};
  graphicsInfo.name = "SimpleGraphicsPipeline";
  graphicsInfo.vertexStage.cullType = PrimitiveCullType_None;
  graphicsInfo.vertexStage.primitiveType = PrimitiveType_Triangles;

  VertexLayoutElement vertexElements[2] = {
    // Position
    {
      .name = "Position",
      .binding = 0,
      .location = 0,
      .type = Type_Float32x3,
      .offset = 0,
    },
    // Color
    {
      .name = "Color",
      .binding = 0,
      .location = 1,
      .type = Type_Float32x4,
      .offset = sizeof(float) * 3,
    },
  };

  DepthAttatchment depthAttatchment;
  depthAttatchment.loadOp = LoadOp::LoadOp_Clear;
  depthAttatchment.storeOp = StoreOp::StoreOp_Store;
  depthAttatchment.format = Format_Depth32Float;
  graphicsInfo.layout = pipelineLayout;
  graphicsInfo.vertexStage.vertexLayoutElements = {vertexElements[0], vertexElements[1]};
  graphicsInfo.vertexStage.vertexShader = vertexShader;
  graphicsInfo.vertexStage.shaderEntry = "main";
  graphicsInfo.fragmentStage.fragmentShader = fragmentShader;
  graphicsInfo.fragmentStage.shaderEntry = "main";
  graphicsInfo.fragmentStage.colorAttatchments = {colorAttatchment};
  graphicsInfo.fragmentStage.depthAttatchment = &depthAttatchment;

  auto graphicsPipeline = renderGraph->createGraphicsPipeline(graphicsInfo);

  TextureInfo depthCreateInfo;
  depthCreateInfo.name = "DepthTexture";
  depthCreateInfo.width = 800;
  depthCreateInfo.height = 800;
  depthCreateInfo.format = Format::Format_Depth32Float;
  depthCreateInfo.memoryProperties = BufferUsage::BufferUsage_None;
  depthCreateInfo.usage = ImageUsage::ImageUsage_DepthStencilAttachment; // | ImageUsage::ImageUsage_Sampled;
  Texture depthTexture = renderGraph->createTexture(depthCreateInfo);

  uint32_t imageIndex = 0;
  uint32_t imagesCount = rhi->getSwapChainImagesCount(swapChain);

  DepthStencilAttachmentInfo depthStencilAttatchment;
  depthStencilAttatchment.name = "DepthAttatchment";
  depthStencilAttatchment.clearDepth = 0;
  depthStencilAttatchment.clearStencil = 0;
  depthStencilAttatchment.view = TextureView{
    .texture = depthTexture,
    .access = AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE | AccessPattern::DEPTH_STENCIL_ATTACHMENT_READ,
    .layout = ResourceLayout::DEPTH_STENCIL_ATTACHMENT,
    .index = 0,
    .flags = ImageAspectFlags::Depth,
    .baseArrayLayer = 0,
    .baseMipLevel = 0,
    .layerCount = 1,
    .levelCount = 1,
  };

  float deltaTime = 0;
  while (!window->shouldClose())
  {
    auto frameStart = lib::time::TimeSpan::now();

    ColorAttachmentInfo attatchment;
    attatchment.name = "ColorAttatchment";
    attatchment.view = rhi->getCurrentSwapChainTextureView(swapChain, imageIndex++ % imagesCount);
    attatchment.clearValue = Color::rgb(0, 1, 1, 1);

    RenderPassInfo renderPass;
    renderPass.name = "RenderPass";
    renderPass.scissor = Rect2D(0, 0, 800, 800);
    renderPass.viewport = Viewport(800, 800);
    renderPass.colorAttachments = {
      attatchment,
    };

    renderPass.depthStencilAttachment = &depthStencilAttatchment;

    RHICommandBuffer commandBuffer;
    commandBuffer.cmdBindGraphicsPipeline(graphicsPipeline);
    commandBuffer.cmdBeginRenderPass(renderPass);
    commandBuffer.cmdBindVertexBuffer(
        0,
        BufferView{
          .buffer = vertexBuffer,
          .access = AccessPattern::VERTEX_ATTRIBUTE_READ,
          .offset = 0,
          .size = sizeof(verticeData),
        });
    commandBuffer.cmdDraw(3, 1, 0, 0);
    commandBuffer.cmdEndRenderPass();

    renderGraph->enqueuePass("DrawTrianglePlass", commandBuffer);
    renderGraph->compile();

    RenderGraph::Frame frame;
    renderGraph->run(frame);
    renderGraph->waitFrame(frame);

    auto frameEnd = lib::time::TimeSpan::now();

    window->update();
    deltaTime = (frameEnd - frameStart).milliseconds();

    os::Logger::logf("Frame rate = %f, Delta time = %fms\n", (1000.0f / deltaTime), deltaTime);
  }

  rhi->waitIdle();

  renderGraph->removeSwapChainImages(swapChain);
  rhi->destroySwapChain(swapChain);

  os::Logger::shutdown();
  return 0;
}