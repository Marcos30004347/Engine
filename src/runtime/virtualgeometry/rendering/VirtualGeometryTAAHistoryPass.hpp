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

class VirtualGeometryTAAHistoryPass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t sourceViewPortWidth = 1920u;
    uint32_t sourceViewPortHeight = 1080u;
    uint32_t outputViewPortWidth = 1920u;
    uint32_t outputViewPortHeight = 1080u;
  };

  VirtualGeometryTAAHistoryPass(
      VirtualGeometryScene &scene,
      rendering::Texture resolvedColorTexture,
      rendering::Texture currentGeometryNormalTexture,
      rendering::Texture sceneDepthTexture,
      rendering::Texture packedGeometryIdsLoTexture,
      rendering::Texture packedGeometryIdsHiTexture,
      rendering::Texture historyColorTexture,
      rendering::Texture historyDepthTexture,
      rendering::Texture historyIdsTexture,
      rendering::Texture historyNormalTexture,
      Settings settings)
      : scene_(scene), resolvedColorTexture_(resolvedColorTexture), currentGeometryNormalTexture_(currentGeometryNormalTexture), sceneDepthTexture_(sceneDepthTexture),
        packedGeometryIdsLoTexture_(packedGeometryIdsLoTexture), packedGeometryIdsHiTexture_(packedGeometryIdsHiTexture), historyColorTexture_(historyColorTexture),
        historyDepthTexture_(historyDepthTexture), historyIdsTexture_(historyIdsTexture), historyNormalTexture_(historyNormalTexture), settings_(settings)
  {
  }

  ~VirtualGeometryTAAHistoryPass() override
  {
    destroyFrameOverrideBuffers();
    renderGraph->deleteComputePipeline(pipeline_);
    renderGraph->deleteShader(shader_);
    renderGraph->deleteBindingGroups(bindingGroups_);
    renderGraph->deleteBindingsLayout(layout_);
  }

  void updateUniforms(const math::Vec2f &currentJitter)
  {
    Uniforms uniforms{};
    uniforms.sourceViewport[0] = settings_.sourceViewPortWidth;
    uniforms.sourceViewport[1] = settings_.sourceViewPortHeight;
    uniforms.outputViewport[0] = settings_.outputViewPortWidth;
    uniforms.outputViewport[1] = settings_.outputViewPortHeight;
    uniforms.currentJitter[0] = currentJitter[0];
    uniforms.currentJitter[1] = currentJitter[1];

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

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    uniformBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_Uniforms.buffer",
            .size = sizeof(Uniforms),
            .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });
    initializeFrameOverrideBuffers();

    layout_ = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
            .name = passName + "_Layout",
            .groups = {rendering::BindingGroupLayout{
                .buffers = {
                    {.name = "taaUniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "instances", .binding = 5, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "pageTable", .binding = 6, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "pagesBuffer", .binding = 7, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .textures = {
                    {.name = "resolvedColorTexture", .binding = 1, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "currentGeometryNormalTexture", .binding = 2, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "sceneDepthTexture", .binding = 3, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "packedGeometryIdsLoTexture", .binding = 4, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "packedGeometryIdsHiTexture", .binding = 12, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .storageTextures = {
                    {.name = "historyColorTexture", .binding = 8, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "historyDepthTexture", .binding = 9, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "historyIdsTexture", .binding = 10, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "historyNormalTexture", .binding = 11, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
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
                    {.binding = 5, .bufferView = {.buffer = scene_.instanceBuffer, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = scene_.getInstanceCount() * sizeof(VirtualGeometryInstanceGPUData)}},
                    {.binding = 6, .bufferView = {.buffer = scene_.pageTableBuffer, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = scene_.pagesTableBufferSize}},
                    {.binding = 7, .bufferView = {.buffer = scene_.pagesBuffer, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = scene_.pagesBufferSize}},
                },
                .textures = {
                    {.binding = 1, .textureView = makeSampledColorView(resolvedColorTexture_)},
                    {.binding = 2, .textureView = makeSampledColorView(currentGeometryNormalTexture_)},
                    {.binding = 3, .textureView = makeSampledDepthView(sceneDepthTexture_)},
                    {.binding = 4, .textureView = makeSampledColorView(packedGeometryIdsLoTexture_)},
                    {.binding = 12, .textureView = makeSampledColorView(packedGeometryIdsHiTexture_)},
                },
                .storageTextures = {
                    {.binding = 8, .textureView = makeStorageColorView(historyColorTexture_)},
                    {.binding = 9, .textureView = makeStorageColorView(historyDepthTexture_)},
                    {.binding = 10, .textureView = makeStorageColorView(historyIdsTexture_)},
                    {.binding = 11, .textureView = makeStorageColorView(historyNormalTexture_)},
                },
            }},
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/virtualgeometry-taa-history-update-cs.spirv"),
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
    float _padding0[2]{};
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
  rendering::Texture resolvedColorTexture_;
  rendering::Texture currentGeometryNormalTexture_;
  rendering::Texture sceneDepthTexture_;
  rendering::Texture packedGeometryIdsLoTexture_;
  rendering::Texture packedGeometryIdsHiTexture_;
  rendering::Texture historyColorTexture_;
  rendering::Texture historyDepthTexture_;
  rendering::Texture historyIdsTexture_;
  rendering::Texture historyNormalTexture_;
  Settings settings_;

  rendering::BindingsLayout layout_;
  rendering::BindingGroups bindingGroups_;
  rendering::Shader shader_;
  rendering::ComputePipeline pipeline_;
  rendering::Buffer uniformBuffer_;
  std::vector<rendering::BufferId> frameOverrideUniformBufferIds_;
};

} // namespace gpgpu
} // namespace virtualgeometry
