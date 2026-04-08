#include <iostream>

#include "os/File.hpp"
#include "os/Logger.hpp"

#include "math/math.hpp"
#include "rendering/core/Camera.hpp"
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

  constexpr uint32_t kWidth = 800;
  constexpr uint32_t kHeight = 800;

  window::Window *window = new window::sdl3::SDL3Window(window::WindowSurface_Vulkan, "Triangle — Reverse-Z Perspective", kWidth, kHeight);

  DeviceRequiredLimits limits = {
    .minimumMemory = 0,
    .minimumComputeSharedMemory = 0,
    .minimumComputeWorkGroupInvocations = 0,
  };

  DeviceFeatures features = DeviceFeatures::DeviceFeatures_Compute;

  vulkan::VulkanRHI *rhi = new vulkan::VulkanRHI(vulkan::Vulkan_1_2, limits, features, {});

  std::vector<VkSurfaceKHR> surfaces;
  surfaces.push_back(window->getVulkanSurface(rhi->getInstance()));
  rhi->init(surfaces);

  SwapChain swapChain = rhi->createSwapChain(0, kWidth, kHeight);

  RenderGraph *renderGraph = new RenderGraph(rhi);
  renderGraph->addSwapChainImages(swapChain);

  float vertexData[] = {
    //  x      y      z      r     g     b     a
    0.0f,  0.5f,  -2.0f, 1.0f, 0.0f, 0.0f, 1.0f, // top    — red
    0.5f,  -0.5f, -2.0f, 0.0f, 1.0f, 0.0f, 1.0f, // right  — green
    -0.5f, -0.5f, -2.0f, 0.0f, 0.0f, 1.0f, 1.0f, // left   — blue
  };

  Buffer vertexBuffer = renderGraph->createBuffer(
      BufferInfo{
        .name = "Triangle.vertexBuffer",
        .size = sizeof(vertexData),
        .usage = BufferUsage::BufferUsage_Vertex | BufferUsage::BufferUsage_Push,
      });
  renderGraph->bufferWrite(vertexBuffer, 0, sizeof(vertexData), (void **)&vertexData);

  constexpr float kFovY = 60.0f * (3.14159265f / 180.0f);
  constexpr float kAspect = static_cast<float>(kWidth) / static_cast<float>(kHeight);
  constexpr float kNear = 0.1f;
  constexpr float kFar = 1000.0f;
  constexpr bool kReverseZ = true;
  constexpr float kClearDepth = 0.0f;

  math::Vec3f camPos(0.0f, 0.0f, 0.0f);
  math::Vec3f camForward(0.0f, 0.0f, -1.0f);

  rendering::Camera cam(kFovY, camPos, camForward, kReverseZ);
  cam.setAspectRatio(kAspect);
  cam.setNearFar(kNear, kFar);
  cam.updateMatrices();

  os::Logger::logf("Camera pos     : (%.2f, %.2f, %.2f)", camPos[0], camPos[1], camPos[2]);
  os::Logger::logf("Camera forward : (%.2f, %.2f, %.2f)", camForward[0], camForward[1], camForward[2]);
  os::Logger::logf("ReverseZ       : %s", kReverseZ ? "true" : "false");
  os::Logger::logf("ClearDepth     : %.2f (0.0 = far plane in reverse-Z)", kClearDepth);
  os::Logger::logf("DepthCompare   : ComparisonOp_GreaterOrEqual (required for reverse-Z)");

  // -------------------------------------------------------------------------
  // Uniform buffer — holds view + proj matrices (column-major, 2 × mat4).
  // -------------------------------------------------------------------------
  struct Uniforms
  {
    float view[16];
    float proj[16];
  };

  Buffer uniformBuffer = renderGraph->createBuffer(
      BufferInfo{
        .name = "Triangle.uniformBuffer",
        .size = sizeof(Uniforms),
        .usage = BufferUsage::BufferUsage_Uniform | BufferUsage::BufferUsage_Push,
      });

  // -------------------------------------------------------------------------
  // Shaders — reuse the same triangle spirv; the vertex shader must read
  // a uniform block with view+proj and multiply them in.
  // If the existing triangle shaders are hardcoded to clip-space coords,
  // replace the spirv paths with ones that include the MVP transform.
  // -------------------------------------------------------------------------
  std::string vertSrc = os::io::readRelativeFile("assets/shaders/triangle/spirv/vertexReverseZ.spirv");
  std::string fragSrc = os::io::readRelativeFile("assets/shaders/triangle/spirv/fragment.spirv");

  // -------------------------------------------------------------------------
  // Pipeline layout — binding 0 = uniform buffer (view/proj)
  // -------------------------------------------------------------------------
  auto pipelineLayout = renderGraph->createBindingsLayout(
      BindingsLayoutInfo{
        .name = "Triangle.layout",
        .groups =
            {
              BindingGroupLayout{
                .buffers =
                    {
                      {.name = "uniforms",
                       .binding = 0,
                       .isDynamic = false,
                       .type = BufferBindingType::BufferBindingType_UniformBuffer,
                       .visibility = BindingVisibility::BindingVisibility_Vertex},
                    },
              },
            },
      });

  auto bindingGroups = renderGraph->createBindingGroups(
      BindingGroupsInfo{
        .layout = pipelineLayout,
        .name = "Triangle.bindingGroups",
        .groups =
            {
              GroupInfo{
                .name = "Group0",
                .buffers =
                    {
                      {.binding = 0,
                       .bufferView =
                           {
                             .buffer = uniformBuffer,
                             .access = AccessPattern::SHADER_READ,
                             .offset = 0,
                             .size = sizeof(Uniforms),
                           }},
                    },
              },
            },
      });

  auto vertexShader = renderGraph->createShader(
      ShaderInfo{
        .name = "Triangle.vertexShader",
        .layout = pipelineLayout,
        .src = vertSrc,
        .type = ShaderType::SpirV,
      });

  auto fragmentShader = renderGraph->createShader(
      ShaderInfo{
        .name = "Triangle.fragmentShader",
        .layout = pipelineLayout,
        .src = fragSrc,
        .type = ShaderType::SpirV,
      });

  // -------------------------------------------------------------------------
  // Depth attachment — GREATER_OR_EQUAL for reverse-Z.
  // -------------------------------------------------------------------------
  DepthAttatchment depthAttachment{};
  depthAttachment.enabled = true;
  depthAttachment.comparison = ComparisonOp::ComparisonOp_GreaterOrEqual;
  depthAttachment.loadOp = LoadOp::LoadOp_Clear;
  depthAttachment.storeOp = StoreOp::StoreOp_Store;
  depthAttachment.format = Format::Format_Depth32Float;

  // -------------------------------------------------------------------------
  // Color attachment
  // -------------------------------------------------------------------------
  ColorAttatchment colorAttachment{};
  colorAttachment.format = rhi->getSwapChainFormat(swapChain);
  colorAttachment.loadOp = LoadOp::LoadOp_Clear;
  colorAttachment.storeOp = StoreOp::StoreOp_Store;

  // -------------------------------------------------------------------------
  // Vertex layout
  // -------------------------------------------------------------------------
  VertexLayoutElement vertexElements[2] = {
    {.name = "Position", .binding = 0, .location = 0, .type = Type_Float32x3, .offset = 0},
    {.name = "Color", .binding = 0, .location = 1, .type = Type_Float32x4, .offset = sizeof(float) * 3},
  };

  // -------------------------------------------------------------------------
  // Graphics pipeline
  // -------------------------------------------------------------------------
  GraphicsPipelineInfo pipelineInfo{};
  pipelineInfo.name = "Triangle.pipeline";
  pipelineInfo.layout = pipelineLayout;
  pipelineInfo.vertexStage.vertexLayoutElements = {vertexElements[0], vertexElements[1]};
  pipelineInfo.vertexStage.vertexShader = vertexShader;
  pipelineInfo.vertexStage.shaderEntry = "main";
  pipelineInfo.vertexStage.cullType = CullMode::Back;
  pipelineInfo.vertexStage.winding = WindingOrder::CW;
  pipelineInfo.vertexStage.primitiveType = PrimitiveType_Triangles;
  pipelineInfo.fragmentStage.fragmentShader = fragmentShader;
  pipelineInfo.fragmentStage.shaderEntry = "main";
  pipelineInfo.fragmentStage.colorAttatchments = {colorAttachment};
  pipelineInfo.fragmentStage.depthAttatchment = depthAttachment;

  auto graphicsPipeline = renderGraph->createGraphicsPipeline(pipelineInfo);

  // -------------------------------------------------------------------------
  // Depth texture
  // -------------------------------------------------------------------------
  Texture depthTexture = renderGraph->createTexture(
      TextureInfo{
        .name = "Triangle.depthTexture",
        .width = kWidth,
        .height = kHeight,
        .format = Format::Format_Depth32Float,
        .memoryProperties = BufferUsage::BufferUsage_None,
        .usage = ImageUsage::ImageUsage_DepthStencilAttachment,
      });

  DepthStencilAttachmentInfo depthStencilInfo{};
  depthStencilInfo.enabled = true;
  depthStencilInfo.name = "Triangle.depthAttachment";
  depthStencilInfo.clearDepth = kClearDepth; // 0.0 = far plane in reverse-Z
  depthStencilInfo.clearStencil = 0;
  depthStencilInfo.view = TextureView{
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

  // -------------------------------------------------------------------------
  // Frame loop
  // -------------------------------------------------------------------------
  float deltaTime = 0.0f;

  while (!window->shouldClose())
  {
    auto frameStart = lib::time::TimeSpan::now();

    {
      const math::Mat4f &view = cam.getViewMatrix();
      const math::Mat4f &proj = cam.getProjectionMatrix();

      Uniforms u{};
      std::memcpy(u.view, view.data, sizeof(float) * 16);
      std::memcpy(u.proj, proj.data, sizeof(float) * 16);

      renderGraph->bufferWrite(uniformBuffer, 0, sizeof(Uniforms), &u);
    }

    ColorAttachmentInfo colorAttachmentInfo{};
    colorAttachmentInfo.name = "Triangle.colorAttachment";
    colorAttachmentInfo.view = rhi->getCurrentSwapChainTextureView(swapChain);
    colorAttachmentInfo.clearValue = Color::rgba(0, 1, 1, 1); // teal background

    RenderPassInfo renderPass{};
    renderPass.name = "Triangle.renderPass";
    renderPass.scissor = Rect2D(0, 0, kWidth, kHeight);
    renderPass.viewport = Viewport(kWidth, kHeight);
    renderPass.colorAttachments = {colorAttachmentInfo};
    renderPass.depthStencilAttachment = depthStencilInfo;

    CommandRecorder commandBuffer;
    commandBuffer.cmdBeginRenderPass(renderPass);
    commandBuffer.cmdBindGraphicsPipeline(graphicsPipeline);
    commandBuffer.cmdBindBindingGroups(bindingGroups, nullptr, 0);
    commandBuffer.cmdBindVertexBuffer(
        0,
        BufferView{
          .buffer = vertexBuffer,
          .access = AccessPattern::VERTEX_ATTRIBUTE_READ,
          .offset = 0,
          .size = sizeof(vertexData),
        });
    commandBuffer.cmdDraw(3, 1, 0, 0);
    commandBuffer.cmdEndRenderPass();

    renderGraph->enqueueCommandBuffer("Triangle.pass", commandBuffer);
    renderGraph->compile();

    RenderGraph::Frame frame;
    RenderGraph::Overrides overrides;
    renderGraph->run(frame, overrides);
    renderGraph->waitFrame(frame);

    auto frameEnd = lib::time::TimeSpan::now();
    window->update();
    deltaTime = (frameEnd - frameStart).milliseconds();

    os::Logger::logf("FPS=%.1f  dt=%.2fms", 1000.0f / deltaTime, deltaTime);
  }

  rhi->waitIdle();
  renderGraph->removeSwapChainImages(swapChain);
  rhi->destroySwapChain(swapChain);
  delete renderGraph;
  os::Logger::shutdown();
  delete window;
  delete rhi;
  return 0;
}