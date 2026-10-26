#pragma once

#include <algorithm>
#include <cstring>
#include <vector>

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"

namespace virtualgeometry
{
namespace gpgpu
{

class VirtualGeometryTAAResolvePass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t sourceViewPortWidth = 1920u;
    uint32_t sourceViewPortHeight = 1080u;
    uint32_t outputViewPortWidth = 1920u;
    uint32_t outputViewPortHeight = 1080u;
    float baseWeight = 0.92f;
    float motionScale = 0.06f;
    float depthThreshold = 0.0025f;
    float normalThreshold = 0.85f;
    float reactiveScale = 4.0f;
    float edgeScale = 800.0f;
    float sharpenStrength = 0.35f;
    float detailBoost = 0.25f;
  };

  VirtualGeometryTAAResolvePass(
      VirtualGeometryScene &scene,
      rendering::Texture currentColorTexture,
      rendering::Texture currentGeometryNormalTexture,
      rendering::Texture sceneDepthTexture,
      rendering::Texture motionVectorTexture,
      rendering::Texture packedGeometryIdsLoTexture,
      rendering::Texture packedGeometryIdsHiTexture,
      rendering::Texture historyColorTexture,
      rendering::Texture historyDepthTexture,
      rendering::Texture historyIdsTexture,
      rendering::Texture historyNormalTexture,
      rendering::Texture outputTexture,
      rendering::Texture disocclusionMaskTexture,
      Settings settings)
      : scene_(scene), currentColorTexture_(currentColorTexture), currentGeometryNormalTexture_(currentGeometryNormalTexture), sceneDepthTexture_(sceneDepthTexture),
        motionVectorTexture_(motionVectorTexture), packedGeometryIdsLoTexture_(packedGeometryIdsLoTexture), packedGeometryIdsHiTexture_(packedGeometryIdsHiTexture),
        historyColorTexture_(historyColorTexture), historyDepthTexture_(historyDepthTexture), historyIdsTexture_(historyIdsTexture), historyNormalTexture_(historyNormalTexture),
        outputTexture_(outputTexture), disocclusionMaskTexture_(disocclusionMaskTexture), settings_(settings)
  {
  }

  ~VirtualGeometryTAAResolvePass() override
  {
    destroyFrameOverrideBuffers();
    renderGraph->deleteComputePipeline(pipeline_);
    renderGraph->deleteShader(shader_);
    renderGraph->deleteBindingGroups(bindingGroups_);
    renderGraph->deleteBindingsLayout(layout_);
    renderGraph->deleteSampler(linearSampler_);
  }

  void updateUniforms(bool historyValid, const math::Vec2f &currentJitter, const math::Vec2f &previousJitter)
  {
    Uniforms uniforms{};
    uniforms.sourceViewport[0] = settings_.sourceViewPortWidth;
    uniforms.sourceViewport[1] = settings_.sourceViewPortHeight;
    uniforms.outputViewport[0] = settings_.outputViewPortWidth;
    uniforms.outputViewport[1] = settings_.outputViewPortHeight;
    uniforms.currentJitter[0] = currentJitter[0];
    uniforms.currentJitter[1] = currentJitter[1];
    uniforms.previousJitter[0] = previousJitter[0];
    uniforms.previousJitter[1] = previousJitter[1];
    uniforms.historyValid = historyValid ? 1u : 0u;
    uniforms.baseWeight = settings_.baseWeight;
    uniforms.motionScale = settings_.motionScale;
    uniforms.depthThreshold = settings_.depthThreshold;
    uniforms.normalThreshold = settings_.normalThreshold;
    uniforms.reactiveScale = settings_.reactiveScale;
    uniforms.edgeScale = settings_.edgeScale;
    uniforms.sharpenStrength = settings_.sharpenStrength;
    uniforms.detailBoost = settings_.detailBoost;

    if (frameOverrideUniformBufferIds_.empty())
    {
      renderGraph->bufferWrite(uniformBuffer_, 0, sizeof(Uniforms), &uniforms);
    }
    else
    {
      renderGraph->getRHI()->bufferWrite(getCurrentFrameOverrideUniformBufferId(), 0, sizeof(Uniforms), &uniforms);
    }
  }

  void appendFrameOverrides(rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const
  {
    if (!frameOverrideUniformBufferIds_.empty())
    {
      overrides.bufferOverrides.emplace(uniformBuffer_.name, rendering::RenderGraphBufferOverride{.bufferId = getFrameOverrideUniformBufferId(frameSlot)});
    }
  }

  const rendering::Texture &getOutputTexture() const
  {
    return outputTexture_;
  }

  const rendering::Texture &getDisocclusionMaskTexture() const
  {
    return disocclusionMaskTexture_;
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    uniformBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_Uniforms.buffer",
            .size = sizeof(Uniforms),
            .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });
    initializeFrameOverrideBuffers();

    linearSampler_ = renderGraph->createSampler(
        rendering::SamplerInfo{
            .name = passName + "_LinearSampler",
            .minFilter = rendering::Filter::Linear,
            .magFilter = rendering::Filter::Linear,
            .addressModeU = rendering::SamplerAddressMode::ClampToEdge,
            .addressModeV = rendering::SamplerAddressMode::ClampToEdge,
            .addressModeW = rendering::SamplerAddressMode::ClampToEdge,
            .anisotropyEnable = false,
            .maxAnisotropy = 1.0f,
            .maxLod = 1.0f,
        });

    layout_ = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
            .name = passName + "_Layout",
            .groups = {rendering::BindingGroupLayout{
                .buffers = {
                    {.name = "taaUniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "instances", .binding = 11, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "pageTable", .binding = 12, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "pagesBuffer", .binding = 13, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .samplers = {
                    {.name = "linearSampler", .binding = 1, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .textures = {
                    {.name = "currentColorTexture", .binding = 2, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "currentGeometryNormalTexture", .binding = 3, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "sceneDepthTexture", .binding = 4, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "motionVectorTexture", .binding = 5, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "packedGeometryIdsLoTexture", .binding = 6, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "packedGeometryIdsHiTexture", .binding = 7, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "historyColorTexture", .binding = 8, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "historyDepthTexture", .binding = 9, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "historyIdsTexture", .binding = 10, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "historyNormalTexture", .binding = 16, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .storageTextures = {
                    {.name = "outputTexture", .binding = 14, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "disocclusionMaskTexture", .binding = 15, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
            }},
        });

    bindingGroups_ = renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
            .layout = layout_,
            .name = passName + "_Groups",
            .groups = {rendering::GroupInfo{
                .name = "group0",
                .buffers = {
                    {.binding = 0, .bufferView = {.buffer = uniformBuffer_, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = sizeof(Uniforms)}},
                    {.binding = 11, .bufferView = {.buffer = scene_.instanceBuffer, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = scene_.getInstanceCount() * sizeof(VirtualGeometryInstanceGPUData)}},
                    {.binding = 12, .bufferView = {.buffer = scene_.pageTableBuffer, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = scene_.pagesTableBufferSize}},
                    {.binding = 13, .bufferView = {.buffer = scene_.pagesBuffer, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = scene_.pagesBufferSize}},
                },
                .samplers = {
                    {.binding = 1, .sampler = linearSampler_, .view = makeSampledColorView(currentColorTexture_)},
                },
                .textures = {
                    {.binding = 2, .textureView = makeSampledColorView(currentColorTexture_)},
                    {.binding = 3, .textureView = makeSampledColorView(currentGeometryNormalTexture_)},
                    {.binding = 4, .textureView = makeSampledDepthView(sceneDepthTexture_)},
                    {.binding = 5, .textureView = makeSampledColorView(motionVectorTexture_)},
                    {.binding = 6, .textureView = makeSampledColorView(packedGeometryIdsLoTexture_)},
                    {.binding = 7, .textureView = makeSampledColorView(packedGeometryIdsHiTexture_)},
                    {.binding = 8, .textureView = makeSampledColorView(historyColorTexture_)},
                    {.binding = 9, .textureView = makeSampledColorView(historyDepthTexture_)},
                    {.binding = 10, .textureView = makeSampledColorView(historyIdsTexture_)},
                    {.binding = 16, .textureView = makeSampledColorView(historyNormalTexture_)},
                },
                .storageTextures = {
                    {.binding = 14, .textureView = makeStorageColorView(outputTexture_)},
                    {.binding = 15, .textureView = makeStorageColorView(disocclusionMaskTexture_)},
                },
            }},
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/virtualgeometry-taa-resolve-cs.spirv"),
            .type = rendering::ShaderType::SpirV,
        });

    pipeline_ = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{
            .entry = "cs_main",
            .layout = layout_,
            .name = passName + "_Pipeline",
            .shader = shader_,
        });

    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0);
    commandBuffer.cmdDispatch((settings_.outputViewPortWidth + 7u) / 8u, (settings_.outputViewPortHeight + 7u) / 8u, 1u);
  }

private:
  struct Uniforms
  {
    uint32_t sourceViewport[2]{1u, 1u};
    uint32_t outputViewport[2]{1u, 1u};
    float currentJitter[2]{0.0f, 0.0f};
    float previousJitter[2]{0.0f, 0.0f};
    uint32_t historyValid = 0u;
    uint32_t _padding0[3]{};
    float baseWeight = 0.92f;
    float motionScale = 0.06f;
    float depthThreshold = 0.0025f;
    float normalThreshold = 0.85f;
    float reactiveScale = 4.0f;
    float edgeScale = 800.0f;
    float sharpenStrength = 0.35f;
    float detailBoost = 0.25f;
  };

  static rendering::TextureView makeSampledColorView(rendering::Texture texture)
  {
    return rendering::TextureView{
        .texture = texture,
        .access = rendering::AccessPattern::SHADER_READ,
        .layout = rendering::ResourceLayout::SHADER_READ_ONLY,
        .index = 0,
        .flags = rendering::ImageAspectFlags::Color,
        .baseArrayLayer = 0,
        .baseMipLevel = 0,
        .layerCount = 1,
        .levelCount = 1,
    };
  }

  static rendering::TextureView makeSampledDepthView(rendering::Texture texture)
  {
    return rendering::TextureView{
        .texture = texture,
        .access = rendering::AccessPattern::SHADER_READ,
        .layout = rendering::ResourceLayout::SHADER_READ_ONLY,
        .index = 0,
        .flags = rendering::ImageAspectFlags::Depth,
        .baseArrayLayer = 0,
        .baseMipLevel = 0,
        .layerCount = 1,
        .levelCount = 1,
    };
  }

  static rendering::TextureView makeStorageColorView(rendering::Texture texture)
  {
    return rendering::TextureView{
        .texture = texture,
        .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE,
        .layout = rendering::ResourceLayout::GENERAL,
        .index = 0,
        .flags = rendering::ImageAspectFlags::Color,
        .baseArrayLayer = 0,
        .baseMipLevel = 0,
        .layerCount = 1,
        .levelCount = 1,
    };
  }

  rendering::Buffer createFrameLocalBuffer(rendering::BufferInfo info) const
  {
    if (renderGraph != nullptr && renderGraph->getMaxFramesInFlight() > 1u)
      info.frameLocal = true;
    return renderGraph->createBuffer(info);
  }

  void initializeFrameOverrideBuffers()
  {
    const uint32_t frameCount = std::max(1u, renderGraph->getMaxFramesInFlight());
    frameOverrideUniformBufferIds_.assign(frameCount, rendering::BufferId::Invalid);
    for (uint32_t frameSlot = 0u; frameSlot < frameCount; ++frameSlot)
    {
      frameOverrideUniformBufferIds_[frameSlot] = renderGraph->getRHI()->createBuffer(
          rendering::BufferInfo{
              .name = passName + "_Uniforms.override_frame" + std::to_string(frameSlot),
              .size = sizeof(Uniforms),
              .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
          });
    }
  }

  void destroyFrameOverrideBuffers()
  {
    if (renderGraph == nullptr)
      return;

    for (const rendering::BufferId bufferId : frameOverrideUniformBufferIds_)
    {
      if (bufferId != rendering::BufferId::Invalid)
      {
        renderGraph->getRHI()->deleteBuffer(bufferId);
      }
    }
    frameOverrideUniformBufferIds_.clear();
  }

  rendering::BufferId getFrameOverrideUniformBufferId(uint32_t frameSlot) const
  {
    if (frameOverrideUniformBufferIds_.empty())
      return rendering::BufferId::Invalid;
    return frameOverrideUniformBufferIds_[frameSlot % frameOverrideUniformBufferIds_.size()];
  }

  rendering::BufferId getCurrentFrameOverrideUniformBufferId() const
  {
    return getFrameOverrideUniformBufferId(renderGraph == nullptr ? 0u : renderGraph->getCurrentFrameIndex());
  }

  VirtualGeometryScene &scene_;
  rendering::Texture currentColorTexture_;
  rendering::Texture currentGeometryNormalTexture_;
  rendering::Texture sceneDepthTexture_;
  rendering::Texture motionVectorTexture_;
  rendering::Texture packedGeometryIdsLoTexture_;
  rendering::Texture packedGeometryIdsHiTexture_;
  rendering::Texture historyColorTexture_;
  rendering::Texture historyDepthTexture_;
  rendering::Texture historyIdsTexture_;
  rendering::Texture historyNormalTexture_;
  rendering::Texture outputTexture_;
  rendering::Texture disocclusionMaskTexture_;
  Settings settings_;

  rendering::BindingsLayout layout_;
  rendering::BindingGroups bindingGroups_;
  rendering::Shader shader_;
  rendering::ComputePipeline pipeline_;
  rendering::Buffer uniformBuffer_;
  std::vector<rendering::BufferId> frameOverrideUniformBufferIds_;
  rendering::Sampler linearSampler_;
};

} // namespace gpgpu
} // namespace virtualgeometry
