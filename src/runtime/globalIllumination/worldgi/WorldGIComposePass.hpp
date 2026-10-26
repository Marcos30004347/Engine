#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "globalIllumination/worldgi/WorldGITypes.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace globalillumination::worldgi
{

class WorldGIComposePass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t viewPortWidth = 1920u;
    uint32_t viewPortHeight = 1080u;
    bool enabled = false;
    bool diffuseEnabled = true;
    bool glossyEnabled = true;
    bool indirectEnabled = true;
    float diffuseWeight = 1.0f;
    float glossyWeight = 1.0f;
    float indirectIntensity = 1.0f;
    float clampValue = 32.0f;
    float glossyMaxRoughness = 0.65f;
  };

  WorldGIComposePass(
      rendering::Texture diffuseSourceTexture,
      rendering::Texture glossySourceTexture,
      rendering::Texture materialPropertiesTexture,
      rendering::Texture diffuseLightingTexture,
      rendering::Texture glossyRadianceTexture,
      rendering::Texture indirectLightingTexture,
      Settings settings)
      : diffuseSourceTexture_(diffuseSourceTexture),
        glossySourceTexture_(glossySourceTexture),
        materialPropertiesTexture_(materialPropertiesTexture),
        diffuseLightingTexture_(diffuseLightingTexture),
        glossyRadianceTexture_(glossyRadianceTexture),
        indirectLightingTexture_(indirectLightingTexture),
        settings_(settings)
  {
  }

  ~WorldGIComposePass() override
  {
    destroyFrameOverrideBuffers();
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

  void setSettings(const Settings &settings)
  {
    settings_ = settings;
    if (uniformBuffer_.isValid())
      writeUniforms();
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
    ensureResources();
    writeUniforms();

    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    commandBuffer.cmdDispatch((std::max(1u, settings_.viewPortWidth) + 7u) / 8u, (std::max(1u, settings_.viewPortHeight) + 7u) / 8u, 1u);
  }

private:
  struct alignas(16) Uniforms
  {
    uint32_t viewport[2]{1u, 1u};
    uint32_t enabled = 0u;
    uint32_t diffuseEnabled = 1u;
    uint32_t glossyEnabled = 1u;
    uint32_t indirectEnabled = 1u;
    float diffuseWeight = 1.0f;
    float glossyWeight = 1.0f;
    float indirectIntensity = 1.0f;
    float clampValue = 32.0f;
    float glossyMaxRoughness = 0.65f;
    float _padding0 = 0.0f;
  };
  static_assert(sizeof(Uniforms) == 48u, "WorldGIComposePass uniforms must match WGSL layout");

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

  static rendering::BufferView makeReadOnlyBufferView(rendering::Buffer buffer, uint64_t size)
  {
    return rendering::BufferView{.buffer = buffer, .offset = 0u, .size = size, .access = rendering::AccessPattern::SHADER_READ};
  }

  rendering::Buffer createFrameLocalBuffer(rendering::BufferInfo info) const
  {
    if (renderGraph != nullptr && renderGraph->getMaxFramesInFlight() > 1u)
      info.frameLocal = true;
    return renderGraph->createBuffer(info);
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;

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
                    {.name = "composeUniforms", .binding = 0u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .textures = {
                    {.name = "diffuseSourceTexture", .binding = 1u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "glossySourceTexture", .binding = 2u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "materialPropertiesTexture", .binding = 3u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .storageTextures = {
                    {.name = "diffuseLightingTexture", .binding = 4u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "glossyRadianceTexture", .binding = 5u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "indirectLightingTexture", .binding = 6u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
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
                    {.binding = 0u, .bufferView = makeReadOnlyBufferView(uniformBuffer_, sizeof(Uniforms))},
                },
                .textures = {
                    {.binding = 1u, .textureView = makeSampledColorView(diffuseSourceTexture_)},
                    {.binding = 2u, .textureView = makeSampledColorView(glossySourceTexture_)},
                    {.binding = 3u, .textureView = makeSampledColorView(materialPropertiesTexture_)},
                },
                .storageTextures = {
                    {.binding = 4u, .textureView = makeStorageColorView(diffuseLightingTexture_)},
                    {.binding = 5u, .textureView = makeStorageColorView(glossyRadianceTexture_)},
                    {.binding = 6u, .textureView = makeStorageColorView(indirectLightingTexture_)},
                },
            }},
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/globalIllumination/worldgi/worldgi-compose-cs.spirv"),
            .type = rendering::ShaderType::SpirV,
        });

    pipeline_ = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{
            .entry = "cs_main",
            .layout = layout_,
            .name = passName + "_Pipeline",
            .shader = shader_,
        });
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
        renderGraph->getRHI()->deleteBuffer(bufferId);
    }
    frameOverrideUniformBufferIds_.clear();
  }

  rendering::BufferId getFrameOverrideUniformBufferId(uint32_t frameSlot) const
  {
    if (frameOverrideUniformBufferIds_.empty())
      return rendering::BufferId::Invalid;
    return frameOverrideUniformBufferIds_[frameSlot % frameOverrideUniformBufferIds_.size()];
  }

  void writeUniforms()
  {
    Uniforms uniforms{};
    uniforms.viewport[0] = std::max(1u, settings_.viewPortWidth);
    uniforms.viewport[1] = std::max(1u, settings_.viewPortHeight);
    uniforms.enabled = settings_.enabled ? 1u : 0u;
    uniforms.diffuseEnabled = settings_.diffuseEnabled ? 1u : 0u;
    uniforms.glossyEnabled = settings_.glossyEnabled ? 1u : 0u;
    uniforms.indirectEnabled = settings_.indirectEnabled ? 1u : 0u;
    uniforms.diffuseWeight = settings_.diffuseWeight;
    uniforms.glossyWeight = settings_.glossyWeight;
    uniforms.indirectIntensity = settings_.indirectIntensity;
    uniforms.clampValue = settings_.clampValue;
    uniforms.glossyMaxRoughness = settings_.glossyMaxRoughness;

    if (frameOverrideUniformBufferIds_.empty())
    {
      renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(Uniforms), &uniforms);
    }
    else
    {
      for (const rendering::BufferId bufferId : frameOverrideUniformBufferIds_)
      {
        renderGraph->getRHI()->bufferWrite(bufferId, 0u, sizeof(Uniforms), &uniforms);
      }
    }
  }

  rendering::Texture diffuseSourceTexture_{};
  rendering::Texture glossySourceTexture_{};
  rendering::Texture materialPropertiesTexture_{};
  rendering::Texture diffuseLightingTexture_{};
  rendering::Texture glossyRadianceTexture_{};
  rendering::Texture indirectLightingTexture_{};
  Settings settings_{};

  rendering::BindingsLayout layout_{};
  rendering::BindingGroups bindingGroups_{};
  rendering::Shader shader_{};
  rendering::ComputePipeline pipeline_{};
  rendering::Buffer uniformBuffer_{};
  std::vector<rendering::BufferId> frameOverrideUniformBufferIds_{};
};

} // namespace globalillumination::worldgi
