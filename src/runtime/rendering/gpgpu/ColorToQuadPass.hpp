#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

using namespace rendering;

namespace rendering
{
namespace passes
{

class ColorQuadPass : public Pass
{
public:
  struct Settings
  {
    uint32_t viewPortWidth = 1920;
    uint32_t viewPortHeight = 1080;
    rendering::Rect2D region = rendering::Rect2D(0, 0, 1920, 1080);
    rendering::Color color = Color::rgba(0, 0, 0, 1);
  };

private:
  rendering::Texture outputTexture;
  rendering::Format outputFormat;

  rendering::BindingsLayout quadLayout;
  rendering::BindingGroups quadBindingGroups;
  rendering::Shader vertexShader;
  rendering::Shader fragmentShader;
  rendering::GraphicsPipeline graphicsPipeline;
  rendering::Buffer uniformBuffer;

  struct QuadUniforms
  {
    float ndcMinX, ndcMinY;
    float ndcMaxX, ndcMaxY;
    float r, g, b, a;
  };
  static_assert(sizeof(QuadUniforms) == 32);

  static QuadUniforms buildUniforms(const Settings &s)
  {
    const float w = static_cast<float>(s.viewPortWidth);
    const float h = static_cast<float>(s.viewPortHeight);

    QuadUniforms u{};
    u.ndcMinX = (static_cast<float>(s.region.x) / w) * 2.0f - 1.0f;
    u.ndcMinY = (static_cast<float>(s.region.y) / h) * 2.0f - 1.0f;
    u.ndcMaxX = (static_cast<float>(s.region.x + s.region.width) / w) * 2.0f - 1.0f;
    u.ndcMaxY = (static_cast<float>(s.region.y + s.region.height) / h) * 2.0f - 1.0f;
    u.r = s.color.R;
    u.g = s.color.G;
    u.b = s.color.B;
    u.a = s.color.A;
    return u;
  }

public:
  Settings settings;

  ColorQuadPass(rendering::Texture outputTexture, rendering::Format outputFormat, Settings settings) : outputTexture(outputTexture), outputFormat(outputFormat), settings(settings)
  {
  }

  void updateSettings(const Settings &newSettings)
  {
    settings = newSettings;
    const QuadUniforms u = buildUniforms(settings);
    renderGraph->bufferWrite(uniformBuffer, 0, sizeof(QuadUniforms), const_cast<QuadUniforms *>(&u));
  }

  ~ColorQuadPass() override
  {
    renderGraph->deleteGraphicsPipeline(graphicsPipeline);
    renderGraph->deleteShader(vertexShader);
    renderGraph->deleteShader(fragmentShader);
    renderGraph->deleteBindingGroups(quadBindingGroups);
    renderGraph->deleteBindingsLayout(quadLayout);
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    uniformBuffer = createFrameLocalBuffer(
        BufferInfo{
          .name = passName + "_Uniforms.buffer",
          .size = sizeof(QuadUniforms),
          .usage = BufferUsage::BufferUsage_Uniform | BufferUsage::BufferUsage_Push,
        });

    {
      const QuadUniforms u = buildUniforms(settings);
      renderGraph->bufferWrite(uniformBuffer, 0, sizeof(QuadUniforms), const_cast<QuadUniforms *>(&u));
    }

    quadLayout = renderGraph->createBindingsLayout(
        BindingsLayoutInfo{
          .name = passName + "_layout.layout",
          .groups =
              {
                BindingGroupLayout{
                  .buffers =
                      {
                        {
                          .name = "uniforms",
                          .binding = 0,
                          .isDynamic = false,
                          .type = BufferBindingType::BufferBindingType_UniformBuffer,
                          .visibility = BindingVisibility::BindingVisibility_Vertex | BindingVisibility::BindingVisibility_Fragment,
                        },
                      },
                },
              },
        });

    // ---- Binding groups ----
    quadBindingGroups = renderGraph->createBindingGroups(
        BindingGroupsInfo{
          .layout = quadLayout,
          .name = passName + "_bindingGroups",
          .groups =
              {
                GroupInfo{
                  .name = "Group0",
                  .buffers =
                      {
                        {
                          .binding = 0,
                          .bufferView =
                              {
                                .buffer = uniformBuffer,
                                .access = AccessPattern::SHADER_READ,
                                .offset = 0,
                                .size = sizeof(QuadUniforms),
                              },
                        },
                      },
                },
              },
        });

    auto vsSrc = os::io::readRelativeFile("assets/shaders/spirv/renderToQuadPass-vs.spirv");
    auto fsSrc = os::io::readRelativeFile("assets/shaders/spirv/renderColorToQuadPass-fs.spirv");

    vertexShader = renderGraph->createShader(
        ShaderInfo{
          .name = passName + "_vs.shader",
          .layout = quadLayout,
          .src = vsSrc,
          .type = ShaderType::SpirV,
        });

    fragmentShader = renderGraph->createShader(
        ShaderInfo{
          .name = passName + "_fs.shader",
          .layout = quadLayout,
          .src = fsSrc,
          .type = ShaderType::SpirV,
        });

    GraphicsPipelineInfo pipelineInfo{};
    pipelineInfo.name = passName + "_Pipeline";
    pipelineInfo.layout = quadLayout;

    pipelineInfo.vertexStage.vertexLayoutElements = {};
    pipelineInfo.vertexStage.vertexShader = vertexShader;
    pipelineInfo.vertexStage.shaderEntry = "vs_main";
    pipelineInfo.vertexStage.cullType = CullMode::None;
    pipelineInfo.vertexStage.winding = WindingOrder::CCW;
    pipelineInfo.vertexStage.primitiveType = PrimitiveType_Triangles;

    pipelineInfo.fragmentStage.fragmentShader = fragmentShader;
    pipelineInfo.fragmentStage.shaderEntry = "fs_main";
    pipelineInfo.fragmentStage.colorAttatchments = {
      {
        .format = outputFormat,
        .loadOp = LoadOp::LoadOp_Load,
        .storeOp = StoreOp::StoreOp_Store,
        .initialLayout = ResourceLayout::COLOR_ATTACHMENT,
        .finalLayout = ResourceLayout::COLOR_ATTACHMENT,
      },
    };
    pipelineInfo.fragmentStage.depthAttatchment.enabled = false;

    graphicsPipeline = renderGraph->createGraphicsPipeline(pipelineInfo);

    ColorAttachmentInfo colorInfo{};
    colorInfo.name = passName + "_ColorAttachment";
    colorInfo.clearValue = Color::rgba(0, 0, 0, 0);
    colorInfo.view = TextureView{
      .texture = outputTexture,
      .access = AccessPattern::COLOR_ATTACHMENT_WRITE,
      .layout = ResourceLayout::COLOR_ATTACHMENT,
      .index = 0,
      .flags = ImageAspectFlags::Color,
      .baseArrayLayer = 0,
      .baseMipLevel = 0,
      .layerCount = 1,
      .levelCount = 1,
    };

    RenderPassInfo renderPass{};
    renderPass.name = passName + "_RenderPass";
    renderPass.scissor = Rect2D(0, 0, settings.viewPortWidth, settings.viewPortHeight);
    renderPass.viewport = Viewport(settings.viewPortWidth, settings.viewPortHeight);
    renderPass.colorAttachments = {colorInfo};
    renderPass.depthStencilAttachment.enabled = false;

    CommandRecorder &cmd = commandBuffer;
    cmd.cmdBindGraphicsPipeline(graphicsPipeline);
    cmd.cmdBeginRenderPass(renderPass);
    cmd.cmdBindBindingGroups(quadBindingGroups, nullptr, 0);
    cmd.cmdDraw(6, 1, 0, 0);
    cmd.cmdEndRenderPass();
  }

};

} // namespace passes
} // namespace rendering
