#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "globalIllumination/worldgi/WorldGITypes.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsTraceTypes.hpp"
#include "math/math.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace globalillumination::worldgi
{

class WorldGIRayGenPass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t viewPortWidth = 1920u;
    uint32_t viewPortHeight = 1080u;
    uint32_t rayGridWidth = 480u;
    uint32_t rayGridHeight = 270u;
    uint32_t raysPerPixel = 2u;
    uint32_t frameIndex = 0u;
    bool enabled = false;
    bool diffuseEnabled = true;
    bool glossyEnabled = true;
    bool reverseZ = true;
    float minTraceDistance = 0.02f;
    float maxTraceDistance = 64.0f;
    float normalBias = 0.03f;
    float glossyMaxRoughness = 0.65f;
  };

  WorldGIRayGenPass(
      rendering::Texture sceneDepthTexture,
      rendering::Texture geometryNormalTexture,
      rendering::Texture baseColorTexture,
      rendering::Texture materialPropertiesTexture,
      rendering::Buffer worldRayBuffer,
      rendering::Buffer sdfRayBuffer,
      Settings settings)
      : sceneDepthTexture_(sceneDepthTexture),
        geometryNormalTexture_(geometryNormalTexture),
        baseColorTexture_(baseColorTexture),
        materialPropertiesTexture_(materialPropertiesTexture),
        worldRayBuffer_(worldRayBuffer),
        sdfRayBuffer_(sdfRayBuffer),
        settings_(sanitizeSettings(settings))
  {
  }

  ~WorldGIRayGenPass() override
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

  void updateUniforms(const math::Mat4f &viewProjection, const math::Mat4f &inverseViewProjection, const math::Vec3f &cameraPosition)
  {
    viewProjection_ = viewProjection;
    inverseViewProjection_ = inverseViewProjection;
    cameraPosition_ = cameraPosition;
    if (uniformBuffer_.isValid())
      writeUniforms();
  }

  void appendFrameOverrides(rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const
  {
    if (!frameOverrideUniformBufferIds_.empty())
      overrides.bufferOverrides.emplace(uniformBuffer_.name, rendering::RenderGraphBufferOverride{.bufferId = getFrameOverrideUniformBufferId(frameSlot)});
  }

  static uint32_t rayCount(const Settings &settings)
  {
    const Settings sanitized = sanitizeSettings(settings);
    return sanitized.rayGridWidth * sanitized.rayGridHeight * sanitized.raysPerPixel;
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
    float viewProjection[16]{};
    float inverseViewProjection[16]{};
    float cameraPosition[4]{};
    uint32_t viewport[2]{1u, 1u};
    uint32_t rayGrid[2]{1u, 1u};
    uint32_t frameIndex = 0u;
    uint32_t raysPerPixel = 1u;
    uint32_t enabled = 0u;
    uint32_t reverseZ = 1u;
    uint32_t diffuseEnabled = 1u;
    uint32_t glossyEnabled = 1u;
    float minTraceDistance = 0.02f;
    float maxTraceDistance = 64.0f;
    float normalBias = 0.03f;
    float glossyMaxRoughness = 0.65f;
    uint32_t _padding0[2]{0u, 0u};
  };
  static_assert(sizeof(Uniforms) == 208u, "WorldGIRayGenPass uniforms must match WGSL layout");

  static Settings sanitizeSettings(Settings settings)
  {
    settings.viewPortWidth = std::max(1u, settings.viewPortWidth);
    settings.viewPortHeight = std::max(1u, settings.viewPortHeight);
    settings.rayGridWidth = std::max(1u, settings.rayGridWidth);
    settings.rayGridHeight = std::max(1u, settings.rayGridHeight);
    settings.raysPerPixel = std::clamp(settings.raysPerPixel, 1u, 8u);
    settings.minTraceDistance = std::max(0.0f, settings.minTraceDistance);
    settings.maxTraceDistance = std::max(settings.minTraceDistance, settings.maxTraceDistance);
    settings.normalBias = std::max(0.0f, settings.normalBias);
    settings.glossyMaxRoughness = std::clamp(settings.glossyMaxRoughness, 0.0f, 1.0f);
    return settings;
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

  static rendering::BufferView makeBufferView(rendering::Buffer buffer, uint64_t size, rendering::AccessPattern access)
  {
    return rendering::BufferView{.buffer = buffer, .offset = 0u, .size = size, .access = access};
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
                    {.name = "rayGenUniforms", .binding = 0u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "worldGIRays", .binding = 5u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "sdfTraceRays", .binding = 6u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .textures = {
                    {.name = "sceneDepthTexture", .binding = 1u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "geometryNormalTexture", .binding = 2u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "baseColorTexture", .binding = 3u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "materialPropertiesTexture", .binding = 4u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
            }},
        });

    const uint32_t rays = rayCount(settings_);
    bindingGroups_ = renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
            .layout = layout_,
            .name = passName + "_Groups",
            .groups = {rendering::GroupInfo{
                .name = "group0",
                .buffers = {
                    {.binding = 0u, .bufferView = makeBufferView(uniformBuffer_, sizeof(Uniforms), rendering::AccessPattern::SHADER_READ)},
                    {.binding = 5u, .bufferView = makeBufferView(worldRayBuffer_, worldGIRayBufferSize(rays), rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE)},
                    {.binding = 6u, .bufferView = makeBufferView(sdfRayBuffer_, static_cast<uint64_t>(rays) * sizeof(gpusigneddistancefields::gpgpu::TraceRayGPU), rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE)},
                },
                .textures = {
                    {.binding = 1u, .textureView = makeSampledDepthView(sceneDepthTexture_)},
                    {.binding = 2u, .textureView = makeSampledColorView(geometryNormalTexture_)},
                    {.binding = 3u, .textureView = makeSampledColorView(baseColorTexture_)},
                    {.binding = 4u, .textureView = makeSampledColorView(materialPropertiesTexture_)},
                },
            }},
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/globalIllumination/worldgi/worldgi-raygen-cs.spirv"),
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
    std::copy_n(viewProjection_.data, 16u, uniforms.viewProjection);
    std::copy_n(inverseViewProjection_.data, 16u, uniforms.inverseViewProjection);
    uniforms.cameraPosition[0] = cameraPosition_.x();
    uniforms.cameraPosition[1] = cameraPosition_.y();
    uniforms.cameraPosition[2] = cameraPosition_.z();
    uniforms.cameraPosition[3] = 0.0f;
    uniforms.viewport[0] = settings_.viewPortWidth;
    uniforms.viewport[1] = settings_.viewPortHeight;
    uniforms.rayGrid[0] = settings_.rayGridWidth;
    uniforms.rayGrid[1] = settings_.rayGridHeight;
    uniforms.frameIndex = settings_.frameIndex;
    uniforms.raysPerPixel = settings_.raysPerPixel;
    uniforms.enabled = settings_.enabled ? 1u : 0u;
    uniforms.reverseZ = settings_.reverseZ ? 1u : 0u;
    uniforms.diffuseEnabled = settings_.diffuseEnabled ? 1u : 0u;
    uniforms.glossyEnabled = settings_.glossyEnabled ? 1u : 0u;
    uniforms.minTraceDistance = settings_.minTraceDistance;
    uniforms.maxTraceDistance = settings_.maxTraceDistance;
    uniforms.normalBias = settings_.normalBias;
    uniforms.glossyMaxRoughness = settings_.glossyMaxRoughness;
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
  rendering::Texture geometryNormalTexture_{};
  rendering::Texture baseColorTexture_{};
  rendering::Texture materialPropertiesTexture_{};
  rendering::Buffer worldRayBuffer_{};
  rendering::Buffer sdfRayBuffer_{};
  Settings settings_{};
  math::Mat4f viewProjection_ = math::Mat4f::identity();
  math::Mat4f inverseViewProjection_ = math::Mat4f::identity();
  math::Vec3f cameraPosition_ = math::Vec3f(0.0f, 0.0f, 0.0f);

  rendering::BindingsLayout layout_{};
  rendering::BindingGroups bindingGroups_{};
  rendering::Shader shader_{};
  rendering::ComputePipeline pipeline_{};
  rendering::Buffer uniformBuffer_{};
  std::vector<rendering::BufferId> frameOverrideUniformBufferIds_;
};

} // namespace globalillumination::worldgi
