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

class WorldGIHitShadingPass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t viewPortWidth = 1920u;
    uint32_t viewPortHeight = 1080u;
    uint32_t rayGridWidth = 480u;
    uint32_t rayGridHeight = 270u;
    uint32_t raysPerPixel = 2u;
    bool enabled = false;
    bool diffuseEnabled = true;
    bool glossyEnabled = true;
    bool lightGridEnabled = true;
    uint32_t lightGridResolution = kWorldGILightGridDefaultResolution;
    uint32_t lightGridCascadeCount = 6u;
    uint32_t lightGridMaxItemsPerCell = kWorldGILightGridDefaultMaxItemsPerCell;
    uint32_t lightCount = 2u;
    math::Vec3f lightGridCenter = math::Vec3f(0.0f);
    float lightGridBaseExtent = 2000.0f;
    float lightGridCascadeScale = 2.0f;
    float clampValue = 32.0f;
    float diffuseScale = 1.0f;
    float glossyScale = 1.0f;
  };

  WorldGIHitShadingPass(
      rendering::Buffer worldRayBuffer,
      rendering::Buffer traceSurfaceBuffer,
      rendering::Buffer lightBuffer,
      rendering::Buffer lightGridCellBuffer,
      rendering::Buffer lightGridItemIdBuffer,
      rendering::Texture diffuseSeedTexture,
      rendering::Texture glossySeedTexture,
      Settings settings)
      : worldRayBuffer_(worldRayBuffer),
        traceSurfaceBuffer_(traceSurfaceBuffer),
        lightBuffer_(lightBuffer),
        lightGridCellBuffer_(lightGridCellBuffer),
        lightGridItemIdBuffer_(lightGridItemIdBuffer),
        diffuseSeedTexture_(diffuseSeedTexture),
        glossySeedTexture_(glossySeedTexture),
        settings_(sanitizeSettings(settings))
  {
  }

  ~WorldGIHitShadingPass() override
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
    commandBuffer.cmdDispatch((settings_.rayGridWidth + 7u) / 8u, (settings_.rayGridHeight + 7u) / 8u, 1u);
  }

private:
  struct alignas(16) Uniforms
  {
    uint32_t viewport[2]{1u, 1u};
    uint32_t rayGrid[2]{1u, 1u};
    uint32_t raysPerPixel = 1u;
    uint32_t enabled = 0u;
    uint32_t diffuseEnabled = 1u;
    uint32_t glossyEnabled = 1u;
    uint32_t lightGridResolution[3]{kWorldGILightGridDefaultResolution, kWorldGILightGridDefaultResolution, kWorldGILightGridDefaultResolution};
    uint32_t lightGridCascadeCount = 6u;
    uint32_t lightGridMaxItemsPerCell = kWorldGILightGridDefaultMaxItemsPerCell;
    uint32_t lightCount = 2u;
    uint32_t lightGridEnabled = 1u;
    uint32_t _paddingGrid0 = 0u;
    float lightGridCenterBaseExtent[4]{0.0f, 0.0f, 0.0f, 2000.0f};
    float lightGridCascadeScalePadding[4]{2.0f, 0.0f, 0.0f, 0.0f};
    float clampValue = 32.0f;
    float diffuseScale = 1.0f;
    float glossyScale = 1.0f;
    float _padding0 = 0.0f;
  };
  static_assert(sizeof(Uniforms) == 112u, "WorldGIHitShadingPass uniforms must match WGSL layout");

  static Settings sanitizeSettings(Settings settings)
  {
    settings.viewPortWidth = std::max(1u, settings.viewPortWidth);
    settings.viewPortHeight = std::max(1u, settings.viewPortHeight);
    settings.rayGridWidth = std::max(1u, settings.rayGridWidth);
    settings.rayGridHeight = std::max(1u, settings.rayGridHeight);
    settings.raysPerPixel = std::clamp(settings.raysPerPixel, 1u, 8u);
    settings.lightGridResolution = std::clamp(settings.lightGridResolution, 1u, 64u);
    settings.lightGridCascadeCount = std::clamp(settings.lightGridCascadeCount, 1u, kWorldGILightGridMaxCascades);
    settings.lightGridMaxItemsPerCell = std::clamp(settings.lightGridMaxItemsPerCell, 1u, 256u);
    settings.lightCount = std::clamp(settings.lightCount, 1u, kWorldGILightGridMaxLights);
    settings.lightGridBaseExtent = std::max(0.001f, settings.lightGridBaseExtent);
    settings.lightGridCascadeScale = std::max(1.0f, settings.lightGridCascadeScale);
    settings.clampValue = std::max(0.0f, settings.clampValue);
    settings.diffuseScale = std::max(0.0f, settings.diffuseScale);
    settings.glossyScale = std::max(0.0f, settings.glossyScale);
    return settings;
  }

  static rendering::BufferView makeBufferView(rendering::Buffer buffer, uint64_t size, rendering::AccessPattern access)
  {
    return rendering::BufferView{.buffer = buffer, .offset = 0u, .size = size, .access = access};
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
                    {.name = "hitShadingUniforms", .binding = 0u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "worldGIRays", .binding = 1u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "traceSurfaces", .binding = 2u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "worldGILights", .binding = 5u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "worldGILightGridCells", .binding = 6u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "worldGILightGridItemIds", .binding = 7u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .storageTextures = {
                    {.name = "diffuseSeedTexture", .binding = 3u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "glossySeedTexture", .binding = 4u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
            }},
        });

    const uint32_t rays = settings_.rayGridWidth * settings_.rayGridHeight * settings_.raysPerPixel;
    bindingGroups_ = renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
            .layout = layout_,
            .name = passName + "_Groups",
            .groups = {rendering::GroupInfo{
                .name = "group0",
                .buffers = {
                    {.binding = 0u, .bufferView = makeBufferView(uniformBuffer_, sizeof(Uniforms), rendering::AccessPattern::SHADER_READ)},
                    {.binding = 1u, .bufferView = makeBufferView(worldRayBuffer_, worldGIRayBufferSize(rays), rendering::AccessPattern::SHADER_READ)},
                    {.binding = 2u, .bufferView = makeBufferView(traceSurfaceBuffer_, giTraceSurfaceBufferSize(rays), rendering::AccessPattern::SHADER_READ)},
                    {.binding = 5u, .bufferView = makeBufferView(lightBuffer_, worldGILightBufferSize(settings_.lightCount), rendering::AccessPattern::SHADER_READ)},
                    {.binding = 6u, .bufferView = makeBufferView(lightGridCellBuffer_, worldGILightGridCellBufferSize(settings_.lightGridResolution, settings_.lightGridCascadeCount), rendering::AccessPattern::SHADER_READ)},
                    {.binding = 7u, .bufferView = makeBufferView(lightGridItemIdBuffer_, worldGILightGridItemBufferSize(settings_.lightGridResolution, settings_.lightGridCascadeCount, settings_.lightGridMaxItemsPerCell), rendering::AccessPattern::SHADER_READ)},
                },
                .storageTextures = {
                    {.binding = 3u, .textureView = makeStorageColorView(diffuseSeedTexture_)},
                    {.binding = 4u, .textureView = makeStorageColorView(glossySeedTexture_)},
                },
            }},
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/globalIllumination/worldgi/worldgi-hit-shading-cs.spirv"),
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
    uniforms.viewport[0] = settings_.viewPortWidth;
    uniforms.viewport[1] = settings_.viewPortHeight;
    uniforms.rayGrid[0] = settings_.rayGridWidth;
    uniforms.rayGrid[1] = settings_.rayGridHeight;
    uniforms.raysPerPixel = settings_.raysPerPixel;
    uniforms.enabled = settings_.enabled ? 1u : 0u;
    uniforms.diffuseEnabled = settings_.diffuseEnabled ? 1u : 0u;
    uniforms.glossyEnabled = settings_.glossyEnabled ? 1u : 0u;
    uniforms.lightGridResolution[0] = settings_.lightGridResolution;
    uniforms.lightGridResolution[1] = settings_.lightGridResolution;
    uniforms.lightGridResolution[2] = settings_.lightGridResolution;
    uniforms.lightGridCascadeCount = settings_.lightGridCascadeCount;
    uniforms.lightGridMaxItemsPerCell = settings_.lightGridMaxItemsPerCell;
    uniforms.lightCount = settings_.lightCount;
    uniforms.lightGridEnabled = settings_.lightGridEnabled ? 1u : 0u;
    uniforms.lightGridCenterBaseExtent[0] = settings_.lightGridCenter.x();
    uniforms.lightGridCenterBaseExtent[1] = settings_.lightGridCenter.y();
    uniforms.lightGridCenterBaseExtent[2] = settings_.lightGridCenter.z();
    uniforms.lightGridCenterBaseExtent[3] = settings_.lightGridBaseExtent;
    uniforms.lightGridCascadeScalePadding[0] = settings_.lightGridCascadeScale;
    uniforms.clampValue = settings_.clampValue;
    uniforms.diffuseScale = settings_.diffuseScale;
    uniforms.glossyScale = settings_.glossyScale;
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

  rendering::Buffer worldRayBuffer_{};
  rendering::Buffer traceSurfaceBuffer_{};
  rendering::Buffer lightBuffer_{};
  rendering::Buffer lightGridCellBuffer_{};
  rendering::Buffer lightGridItemIdBuffer_{};
  rendering::Texture diffuseSeedTexture_{};
  rendering::Texture glossySeedTexture_{};
  Settings settings_{};

  rendering::BindingsLayout layout_{};
  rendering::BindingGroups bindingGroups_{};
  rendering::Shader shader_{};
  rendering::ComputePipeline pipeline_{};
  rendering::Buffer uniformBuffer_{};
  std::vector<rendering::BufferId> frameOverrideUniformBufferIds_;
};

} // namespace globalillumination::worldgi
