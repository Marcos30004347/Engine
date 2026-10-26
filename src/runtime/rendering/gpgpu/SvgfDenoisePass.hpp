#pragma once

#include <algorithm>
#include <cstdint>

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace rendering
{
namespace gpgpu
{

class SvgfDenoisePass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t width = 1u;
    uint32_t height = 1u;
    uint32_t workgroupSizeX = 8u;
    uint32_t workgroupSizeY = 8u;
    float currentJitter[2] = {0.0f, 0.0f};
    float previousJitter[2] = {0.0f, 0.0f};
    float historyWeight = 0.96f;
    float depthSigma = 0.0025f;
    float normalSigma = 32.0f;
    float luminanceSigma = 2.0f;
    float validRatioMin = 0.25f;
    float maxHistoryFrames = 32.0f;
    float fireflyClamp = 32.0f;
    float spatialSigma = 1.0f;
    float spatialRadius = 1.0f;
    float spatialStrength = 0.65f;
    float disocclusionWeight = 2.0f;
    float minCurrentWeight = 0.25f;
    bool enabled = true;
    bool historyValid = false;
  };

  SvgfDenoisePass(
      rendering::Texture currentTexture,
      rendering::Texture historyTexture,
      rendering::Texture motionVectorTexture,
      rendering::Texture disocclusionMaskTexture,
      rendering::Texture depthTexture,
      rendering::Texture normalTexture,
      rendering::Texture historyDepthTexture,
      rendering::Texture historyNormalTexture,
      rendering::Texture outputTexture,
      Settings settings)
      : currentTexture_(currentTexture),
        historyTexture_(historyTexture),
        motionVectorTexture_(motionVectorTexture),
        disocclusionMaskTexture_(disocclusionMaskTexture),
        depthTexture_(depthTexture),
        normalTexture_(normalTexture),
        historyDepthTexture_(historyDepthTexture),
        historyNormalTexture_(historyNormalTexture),
        outputTexture_(outputTexture),
        settings_(settings)
  {
  }

  ~SvgfDenoisePass() override
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
    uint32_t viewport[4] = {1u, 1u, 0u, 0u};
    float jitter[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float params0[4] = {0.96f, 0.0025f, 32.0f, 2.0f};
    float params1[4] = {0.25f, 32.0f, 32.0f, 1.0f};
    float params2[4] = {1.0f, 0.65f, 2.0f, 0.25f};
  };
  static_assert(sizeof(Uniforms) == 80u, "SvgfDenoisePass::Uniforms size mismatch");

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
                    {.name = "currentTexture", .binding = 1, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "historyTexture", .binding = 2, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "motionVectorTexture", .binding = 3, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "disocclusionMaskTexture", .binding = 4, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "depthTexture", .binding = 5, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "normalTexture", .binding = 6, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "historyDepthTexture", .binding = 7, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "historyNormalTexture", .binding = 8, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .storageTextures = {
                    {.name = "outputTexture", .binding = 9, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
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
                        {.binding = 1, .textureView = makeSampledColorView(currentTexture_)},
                        {.binding = 2, .textureView = makeSampledColorView(historyTexture_)},
                        {.binding = 3, .textureView = makeSampledColorView(motionVectorTexture_)},
                        {.binding = 4, .textureView = makeSampledColorView(disocclusionMaskTexture_)},
                        {.binding = 5, .textureView = makeSampledDepthView(depthTexture_)},
                        {.binding = 6, .textureView = makeSampledColorView(normalTexture_)},
                        {.binding = 7, .textureView = makeSampledDepthView(historyDepthTexture_)},
                        {.binding = 8, .textureView = makeSampledColorView(historyNormalTexture_)},
                    },
                    .storageTextures = {
                        {.binding = 9, .textureView = makeStorageColorView(outputTexture_)},
                    },
                },
            },
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/svgf-denoise-cs.spirv"),
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
    const float maxHistoryFrames = std::max(settings_.maxHistoryFrames, 1.0f);
    const Uniforms uniforms{
      .viewport = {std::max(settings_.width, 1u), std::max(settings_.height, 1u), settings_.enabled ? 1u : 0u, settings_.historyValid ? 1u : 0u},
      .jitter = {settings_.currentJitter[0], settings_.currentJitter[1], settings_.previousJitter[0], settings_.previousJitter[1]},
      .params0 = {
          std::clamp(settings_.historyWeight, 0.0f, 0.99f),
          std::max(settings_.depthSigma, 1.0e-6f),
          std::max(settings_.normalSigma, 1.0e-4f),
          std::max(settings_.luminanceSigma, 1.0e-4f),
      },
      .params1 = {
          std::max(settings_.validRatioMin, 1.0e-4f),
          maxHistoryFrames,
          std::max(settings_.fireflyClamp, 1.0e-4f),
          std::max(settings_.spatialSigma, 1.0e-4f),
      },
      .params2 = {
          std::clamp(settings_.spatialRadius, 1.0f, 8.0f),
          std::clamp(settings_.spatialStrength, 0.0f, 1.0f),
          std::max(settings_.disocclusionWeight, 0.0f),
          std::clamp(settings_.minCurrentWeight, 0.01f, 1.0f),
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

  rendering::Texture currentTexture_{};
  rendering::Texture historyTexture_{};
  rendering::Texture motionVectorTexture_{};
  rendering::Texture disocclusionMaskTexture_{};
  rendering::Texture depthTexture_{};
  rendering::Texture normalTexture_{};
  rendering::Texture historyDepthTexture_{};
  rendering::Texture historyNormalTexture_{};
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
