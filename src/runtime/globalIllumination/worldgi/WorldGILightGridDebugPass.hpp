#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "globalIllumination/worldgi/WorldGITypes.hpp"
#include "math/math.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace globalillumination::worldgi
{

class WorldGILightGridDebugPass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t viewPortWidth = 1920u;
    uint32_t viewPortHeight = 1080u;
    bool enabled = false;
    bool reverseZ = true;
    uint32_t gridResolution = kWorldGILightGridDefaultResolution;
    uint32_t cascadeCount = kWorldGILightGridMaxCascades;
    int32_t selectedCascade = -1;
    float lineWidth = 0.035f;
    math::Vec3f gridCenter = math::Vec3f(0.0f);
    float baseExtent = 2000.0f;
    float cascadeScale = 2.0f;
  };

  WorldGILightGridDebugPass(
      rendering::Texture sceneDepthTexture,
      rendering::Texture outputTexture,
      Settings settings)
      : sceneDepthTexture_(sceneDepthTexture),
        outputTexture_(outputTexture),
        settings_(sanitizeSettings(settings))
  {
  }

  ~WorldGILightGridDebugPass() override
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

  void setSettings(Settings settings)
  {
    settings_ = sanitizeSettings(settings);
    if (uniformBuffer_.isValid())
      writeUniforms();
  }

  void updateUniforms(const math::Mat4f &inverseViewProjection)
  {
    inverseViewProjection_ = inverseViewProjection;
    if (uniformBuffer_.isValid())
      writeUniforms();
  }

  void appendFrameOverrides(rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const
  {
    if (!frameOverrideUniformBufferIds_.empty())
      overrides.bufferOverrides.emplace(uniformBuffer_.name, rendering::RenderGraphBufferOverride{.bufferId = getFrameOverrideUniformBufferId(frameSlot)});
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    writeUniforms();

    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    commandBuffer.cmdDispatch((settings_.viewPortWidth + 7u) / 8u, (settings_.viewPortHeight + 7u) / 8u, 1u);
  }

private:
  struct alignas(16) Uniforms
  {
    float inverseViewProjection[16]{};
    uint32_t viewport[2]{1u, 1u};
    uint32_t enabled = 0u;
    uint32_t reverseZ = 1u;
    uint32_t gridResolution = kWorldGILightGridDefaultResolution;
    uint32_t cascadeCount = kWorldGILightGridMaxCascades;
    int32_t selectedCascade = -1;
    float lineWidth = 0.035f;
    float gridCenterBaseExtent[4]{0.0f, 0.0f, 0.0f, 2000.0f};
    float cascadeScalePadding[4]{2.0f, 0.0f, 0.0f, 0.0f};
  };
  static_assert(sizeof(Uniforms) == 128u, "WorldGILightGridDebugPass uniforms must match WGSL layout");

  static Settings sanitizeSettings(Settings settings)
  {
    settings.viewPortWidth = std::max(1u, settings.viewPortWidth);
    settings.viewPortHeight = std::max(1u, settings.viewPortHeight);
    settings.gridResolution = std::clamp(settings.gridResolution, 1u, 64u);
    settings.cascadeCount = std::clamp(settings.cascadeCount, 1u, kWorldGILightGridMaxCascades);
    settings.selectedCascade = std::clamp(settings.selectedCascade, -1, static_cast<int32_t>(settings.cascadeCount) - 1);
    settings.lineWidth = std::clamp(settings.lineWidth, 0.001f, 0.20f);
    settings.baseExtent = std::max(0.001f, settings.baseExtent);
    settings.cascadeScale = std::max(1.0f, settings.cascadeScale);
    return settings;
  }

  static rendering::TextureView makeDepthView(rendering::Texture texture)
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
                    {.name = "debugUniforms", .binding = 0u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .textures = {
                    {.name = "sceneDepthTexture", .binding = 1u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .storageTextures = {
                    {.name = "outputTexture", .binding = 2u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
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
                    {.binding = 1u, .textureView = makeDepthView(sceneDepthTexture_)},
                },
                .storageTextures = {
                    {.binding = 2u, .textureView = makeStorageColorView(outputTexture_)},
                },
            }},
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/globalIllumination/worldgi/worldgi-light-grid-debug-cs.spirv"),
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
    std::copy(std::begin(inverseViewProjection_.data), std::end(inverseViewProjection_.data), std::begin(uniforms.inverseViewProjection));
    uniforms.viewport[0] = settings_.viewPortWidth;
    uniforms.viewport[1] = settings_.viewPortHeight;
    uniforms.enabled = settings_.enabled ? 1u : 0u;
    uniforms.reverseZ = settings_.reverseZ ? 1u : 0u;
    uniforms.gridResolution = settings_.gridResolution;
    uniforms.cascadeCount = settings_.cascadeCount;
    uniforms.selectedCascade = settings_.selectedCascade;
    uniforms.lineWidth = settings_.lineWidth;
    uniforms.gridCenterBaseExtent[0] = settings_.gridCenter.x();
    uniforms.gridCenterBaseExtent[1] = settings_.gridCenter.y();
    uniforms.gridCenterBaseExtent[2] = settings_.gridCenter.z();
    uniforms.gridCenterBaseExtent[3] = settings_.baseExtent;
    uniforms.cascadeScalePadding[0] = settings_.cascadeScale;
    if (frameOverrideUniformBufferIds_.empty())
    {
      renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(uniforms), &uniforms);
    }
    else
    {
      for (const rendering::BufferId bufferId : frameOverrideUniformBufferIds_)
      {
        if (bufferId != rendering::BufferId::Invalid)
          renderGraph->getRHI()->bufferWrite(bufferId, 0u, sizeof(uniforms), &uniforms);
      }
    }
  }

  rendering::Texture sceneDepthTexture_{};
  rendering::Texture outputTexture_{};
  Settings settings_{};
  math::Mat4f inverseViewProjection_ = math::Mat4f::identity();

  rendering::BindingsLayout layout_{};
  rendering::BindingGroups bindingGroups_{};
  rendering::Shader shader_{};
  rendering::ComputePipeline pipeline_{};
  rendering::Buffer uniformBuffer_{};
  std::vector<rendering::BufferId> frameOverrideUniformBufferIds_;
};

} // namespace globalillumination::worldgi
