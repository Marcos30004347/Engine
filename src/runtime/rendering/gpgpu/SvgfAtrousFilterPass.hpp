#pragma once

#include <algorithm>
#include <cstdint>

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace rendering
{
namespace gpgpu
{

class SvgfAtrousFilterPass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t width = 1u;
    uint32_t height = 1u;
    uint32_t stepWidth = 1u;
    uint32_t workgroupSizeX = 8u;
    uint32_t workgroupSizeY = 8u;
    float depthSigma = 0.0025f;
    float normalSigma = 32.0f;
    float luminanceSigma = 2.0f;
    float fireflyClamp = 16.0f;
    float spatialStrength = 0.5f;
    float varianceSigma = 1.0f;
    float maxHistoryFrames = 32.0f;
    bool enabled = true;
    bool outputHitMask = false;
  };

  SvgfAtrousFilterPass(
      rendering::Texture sourceTexture,
      rendering::Texture depthTexture,
      rendering::Texture normalTexture,
      rendering::Texture outputTexture,
      Settings settings)
      : sourceTexture_(sourceTexture), depthTexture_(depthTexture),
        normalTexture_(normalTexture), outputTexture_(outputTexture),
        settings_(settings)
  {
  }

  ~SvgfAtrousFilterPass() override
  {
    destroy();
  }

  void setSettings(const Settings &settings)
  {
    settings_ = settings;
    if (uniformBuffer_.isValid())
    {
      writeUniforms();
    }
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    createResources();
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0);
    commandBuffer.cmdDispatch(
        (settings_.width + settings_.workgroupSizeX - 1u) / settings_.workgroupSizeX,
        (settings_.height + settings_.workgroupSizeY - 1u) / settings_.workgroupSizeY,
        1u);
  }

private:
  struct Uniforms
  {
    uint32_t viewport[4] = {1u, 1u, 0u, 1u};
    float params0[4] = {0.0025f, 32.0f, 2.0f, 16.0f};
    float params1[4] = {0.5f, 1.0f, 32.0f, 0.0f};
  };
  static_assert(sizeof(Uniforms) == 48u, "SvgfAtrousFilterPass::Uniforms size mismatch");

  void destroy()
  {
    if (pipeline_.isValid())
      renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (bindingGroups_.isValid())
      renderGraph->deleteBindingGroups(bindingGroups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    if (uniformBuffer_.isValid())
      renderGraph->deleteBuffer(uniformBuffer_);
  }

  rendering::Buffer createFrameLocalBuffer(rendering::BufferInfo info) const
  {
    if (renderGraph != nullptr && renderGraph->getMaxFramesInFlight() > 1u)
      info.frameLocal = true;
    return renderGraph->createBuffer(info);
  }

  void createResources()
  {
    if (layout_.isValid())
      return;

    uniformBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
          .name = passName + "_Uniforms.buffer",
          .size = sizeof(Uniforms),
          .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });
    writeUniforms();

    layout_ = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
          .name = passName + "_Layout",
          .groups = {rendering::BindingGroupLayout{
              .buffers = {
                {.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
              },
              .textures = {
                {.name = "sourceTexture", .binding = 1, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "depthTexture", .binding = 2, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "normalTexture", .binding = 3, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
              },
              .storageTextures = {
                {.name = "outputTexture", .binding = 4, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
              },
            }},
        });

    bindingGroups_ = renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
          .layout = layout_,
          .name = passName + "_BindingGroups",
          .groups = {
            rendering::GroupInfo{
              .name = "group0",
              .buffers = {
                {.binding = 0, .bufferView = {.buffer = uniformBuffer_, .offset = 0, .size = sizeof(Uniforms), .access = rendering::AccessPattern::SHADER_READ}},
              },
              .textures = {
                {.binding = 1, .textureView = makeSampledColorView(sourceTexture_)},
                {.binding = 2, .textureView = makeSampledDepthView(depthTexture_)},
                {.binding = 3, .textureView = makeSampledColorView(normalTexture_)},
              },
              .storageTextures = {
                {.binding = 4, .textureView = makeStorageColorView(outputTexture_)},
              },
            },
          },
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
          .name = passName + "_Shader",
          .layout = layout_,
          .src = os::io::readRelativeFile("assets/shaders/spirv/svgf-atrous-filter-cs.spirv"),
          .type = rendering::ShaderType::SpirV,
        });

    pipeline_ = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{
          .entry = "main",
          .layout = layout_,
          .name = passName + "_Pipeline",
          .shader = shader_,
        });
  }

  void writeUniforms()
  {
    const Uniforms uniforms{
      .viewport = {std::max(settings_.width, 1u), std::max(settings_.height, 1u), settings_.enabled ? 1u : 0u, std::max(settings_.stepWidth, 1u)},
      .params0 = {
          std::max(settings_.depthSigma, 1.0e-6f),
          std::max(settings_.normalSigma, 1.0e-4f),
          std::max(settings_.luminanceSigma, 1.0e-4f),
          std::max(settings_.fireflyClamp, 1.0e-4f),
      },
      .params1 = {
          std::clamp(settings_.spatialStrength, 0.0f, 1.0f),
          std::max(settings_.varianceSigma, 1.0e-4f),
          std::max(settings_.maxHistoryFrames, 1.0f),
          settings_.outputHitMask ? 1.0f : 0.0f,
      },
    };
    renderGraph->bufferWrite(uniformBuffer_, 0, sizeof(uniforms), const_cast<Uniforms *>(&uniforms));
  }

  static rendering::TextureView makeSampledColorView(rendering::Texture texture)
  {
    return rendering::TextureView{
        .texture = texture,
        .access = rendering::AccessPattern::SHADER_READ,
        .layout = rendering::ResourceLayout::SHADER_READ_ONLY,
        .index = 0u,
        .flags = rendering::ImageAspectFlags::Color,
        .baseArrayLayer = 0u,
        .baseMipLevel = 0u,
        .layerCount = 1u,
        .levelCount = 1u,
    };
  }

  static rendering::TextureView makeSampledDepthView(rendering::Texture texture)
  {
    return rendering::TextureView{
        .texture = texture,
        .access = rendering::AccessPattern::SHADER_READ,
        .layout = rendering::ResourceLayout::SHADER_READ_ONLY,
        .index = 0u,
        .flags = rendering::ImageAspectFlags::Depth,
        .baseArrayLayer = 0u,
        .baseMipLevel = 0u,
        .layerCount = 1u,
        .levelCount = 1u,
    };
  }

  static rendering::TextureView makeStorageColorView(rendering::Texture texture)
  {
    return rendering::TextureView{
        .texture = texture,
        .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE,
        .layout = rendering::ResourceLayout::GENERAL,
        .index = 0u,
        .flags = rendering::ImageAspectFlags::Color,
        .baseArrayLayer = 0u,
        .baseMipLevel = 0u,
        .layerCount = 1u,
        .levelCount = 1u,
    };
  }

  rendering::Texture sourceTexture_{};
  rendering::Texture depthTexture_{};
  rendering::Texture normalTexture_{};
  rendering::Texture outputTexture_{};
  Settings settings_{};
  rendering::BindingsLayout layout_{};
  rendering::BindingGroups bindingGroups_{};
  rendering::Shader shader_{};
  rendering::ComputePipeline pipeline_{};
  rendering::Buffer uniformBuffer_{};
};

} // namespace gpgpu
} // namespace rendering
