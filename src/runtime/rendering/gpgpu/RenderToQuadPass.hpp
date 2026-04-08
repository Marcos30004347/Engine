#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

using namespace rendering;

namespace rendering
{
namespace passes
{

class RenderToQuadPass : public Pass
{
public:
  struct Settings
  {
    uint32_t viewPortWidth = 1920;
    uint32_t viewPortHeight = 1080;

    rendering::Rect2D quad = rendering::Rect2D(0, 0, 1920, 1080);

    uint32_t sourceMipLevel = 0;
    uint32_t sourceLayer = 0;
    rendering::ImageAspectFlags inputAspectFlags = rendering::ImageAspectFlags::Color;
    bool useShadowAtlasDebugShader = false;
    bool useDepthDebugShader = false;
    bool useScalarDebugShader = false;
  };

private:
  rendering::BindingsLayout quadLayout;
  rendering::BindingGroups quadBindingGroups;
  rendering::Shader vertexShader;
  rendering::Shader fragmentShader;
  rendering::GraphicsPipeline graphicsPipeline;

  rendering::Buffer uniformBuffer;
  rendering::Sampler sampler;

  //   rendering::TextureView outputView;
  rendering::TextureView inputView;
  rendering::Texture outputTexture;

  struct QuadUniforms
  {
    float ndcMinX, ndcMinY;
    float ndcMaxX, ndcMaxY;
    float uvMinX, uvMinY;
    float uvMaxX, uvMaxY;
  };

  // -------------------------------------------------------------------------
  // Helpers
  // -------------------------------------------------------------------------
  static QuadUniforms buildUniforms(const Settings &s)
  {
    const float w = static_cast<float>(s.viewPortWidth);
    const float h = static_cast<float>(s.viewPortHeight);

    QuadUniforms u{};
    u.ndcMinX = (static_cast<float>(s.quad.x) / w) * 2.0f - 1.0f;
    u.ndcMinY = (static_cast<float>(s.quad.y) / h) * 2.0f - 1.0f;
    u.ndcMaxX = (static_cast<float>(s.quad.x + s.quad.width) / w) * 2.0f - 1.0f;
    u.ndcMaxY = (static_cast<float>(s.quad.y + s.quad.height) / h) * 2.0f - 1.0f;

    u.uvMinX = 0.0f;
    u.uvMinY = 0.0f;
    u.uvMaxX = 1.0f;
    u.uvMaxY = 1.0f;
    return u;
  }
  rendering::Format outputFormat;
  //   rendering::ResourceLayout outputInitialLayout;
  //   rendering::ResourceLayout outputFinalLayout;

public:
  Settings settings;

  RenderToQuadPass(
      rendering::Texture outputTexture,
      rendering::Format outputFormat,
      //   rendering::ResourceLayout outputInitialLayout,
      //   rendering::ResourceLayout outputFinalLayout,
      // rendering::TextureView outputView,
      rendering::Texture inputTexture,
      uint32_t sourceMipLevel,
      uint32_t sourceLayer,
      Settings settings)
      : outputTexture(outputTexture), /*outputView(outputView), outputInitialLayout(outputInitialLayout), outputFinalLayout(outputFinalLayout), */ settings(settings),
        outputFormat(outputFormat)
  {
    inputView = rendering::TextureView{
      .texture = inputTexture,
      .access = rendering::AccessPattern::SHADER_READ,
      .layout = rendering::ResourceLayout::SHADER_READ_ONLY,
      .index = 0,
      .flags = settings.inputAspectFlags,
      .baseArrayLayer = sourceLayer,
      .baseMipLevel = sourceMipLevel,
      .layerCount = 1,
      .levelCount = 1,
    };
  }

  void updateSettings(const Settings &newSettings)
  {
    settings = newSettings;
    const QuadUniforms u = buildUniforms(settings);
    renderGraph->bufferWrite(uniformBuffer, 0, sizeof(QuadUniforms), const_cast<QuadUniforms *>(&u));
  }

  ~RenderToQuadPass() override
  {
    renderGraph->deleteGraphicsPipeline(graphicsPipeline);
    renderGraph->deleteShader(vertexShader);
    renderGraph->deleteShader(fragmentShader);
    renderGraph->deleteBindingGroups(quadBindingGroups);
    renderGraph->deleteBindingsLayout(quadLayout);
    renderGraph->deleteSampler(sampler);
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

    // ---- Sampler ----
    sampler = renderGraph->createSampler(
        SamplerInfo{
          .name = passName + "_Sampler.sampler",
          .minFilter = Filter::Nearest,
          .magFilter = Filter::Nearest,
          .addressModeU = SamplerAddressMode::ClampToEdge,
          .addressModeV = SamplerAddressMode::ClampToEdge,
          .addressModeW = SamplerAddressMode::ClampToEdge,
          .anisotropyEnable = false,
          .maxAnisotropy = 1.0f,
          .maxLod = 1.0f,
        });

    // ---- Bindings layout ----
    quadLayout = renderGraph->createBindingsLayout(
        BindingsLayoutInfo{
          .name = passName + "_quadLayout.layout",
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
                  .samplers =
                      {
                        {.name = "texSampler", .binding = 1, .visibility = BindingVisibility::BindingVisibility_Fragment},
                      },
                  .textures =
                      {
                        {.name = "inputTexture", .binding = 2, .visibility = BindingVisibility::BindingVisibility_Fragment},
                      },
                },
              },
        });

    // ---- Binding groups ----
    quadBindingGroups = renderGraph->createBindingGroups(
        BindingGroupsInfo{
          .layout = quadLayout,
          .name = passName + "_quadBindingGroups",
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
                               .size = sizeof(QuadUniforms),
                             }},
                      },
                  .samplers =
                      {
                        {.binding = 1, .sampler = sampler, .view = inputView},
                      },
                  .textures =
                      {
                        {.binding = 2, .textureView = inputView},
                      },
                },
              },
        });

    // ---- Shaders ----
    auto vsSrc = os::io::readRelativeFile("assets/shaders/spirv/renderToQuadPass-vs.spirv");
    std::string fsPath = "assets/shaders/spirv/renderToQuadPass-fs.spirv";
    if (settings.useShadowAtlasDebugShader)
    {
      fsPath = "assets/shaders/spirv/renderToQuadShadowDebug-fs.spirv";
    }
    else if (settings.useDepthDebugShader)
    {
      fsPath = "assets/shaders/spirv/renderToQuadDepthDebug-fs.spirv";
    }
    else if (settings.useScalarDebugShader)
    {
      fsPath = "assets/shaders/spirv/renderToQuadScalarDebug-fs.spirv";
    }
    auto fsSrc = os::io::readRelativeFile(fsPath);

    vertexShader = renderGraph->createShader(
        ShaderInfo{
          .name = passName + "_quadVS.shader",
          .layout = quadLayout,
          .src = vsSrc,
          .type = ShaderType::SpirV,
        });

    fragmentShader = renderGraph->createShader(
        ShaderInfo{
          .name = passName + "_quadFS.shader",
          .layout = quadLayout,
          .src = fsSrc,
          .type = ShaderType::SpirV,
        });

    GraphicsPipelineInfo pipelineInfo{};
    pipelineInfo.name = passName + "_QuadPipeline";
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

    CommandRecorder &cmd = commandBuffer;

    ColorAttachmentInfo colorInfo{};
    colorInfo.name = passName + "_ColorAttachment";
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
    }; // outputView;
    
    colorInfo.clearValue = Color::rgba(0, 0, 0, 0);

    RenderPassInfo renderPass{};
    renderPass.name = passName + "_RenderPass";
    renderPass.scissor = Rect2D(0, 0, settings.viewPortWidth, settings.viewPortHeight);
    renderPass.viewport = Viewport(settings.viewPortWidth, settings.viewPortHeight);
    renderPass.colorAttachments = {colorInfo};
    renderPass.depthStencilAttachment.enabled = false;

    cmd.cmdBindGraphicsPipeline(graphicsPipeline);
    cmd.cmdBeginRenderPass(renderPass);
    cmd.cmdBindBindingGroups(quadBindingGroups, nullptr, 0);
    cmd.cmdDraw(6, 1, 0, 0);
    cmd.cmdEndRenderPass();
  }

};

} // namespace passes
} // namespace rendering
