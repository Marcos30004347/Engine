#pragma once

#include <algorithm>
#include <cstdint>

#include "globalIllumination/worldgi/WorldGITypes.hpp"
#include "math/math.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace globalillumination::worldgi
{

class WorldGIRadianceCacheDebugPass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t viewPortWidth = 1920u;
    uint32_t viewPortHeight = 1080u;
    uint32_t frameIndex = 0u;
    bool enabled = false;
    bool reverseZ = true;
    uint32_t cacheCapacity = kWorldGIRadianceCacheDefaultCapacity;
    uint32_t probeCount = kWorldGIRadianceCacheMaxProbeCount;
    uint32_t ttlFrames = kWorldGIRadianceCacheDefaultTTLFrames;
    float cellSize = 0.5f;
  };

  WorldGIRadianceCacheDebugPass(
      rendering::Texture sceneDepthTexture,
      rendering::Buffer cacheEntryBuffer,
      rendering::Texture outputTexture,
      Settings settings)
      : sceneDepthTexture_(sceneDepthTexture),
        cacheEntryBuffer_(cacheEntryBuffer),
        outputTexture_(outputTexture),
        settings_(sanitizeSettings(settings))
  {
  }

  ~WorldGIRadianceCacheDebugPass() override
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
    uint32_t frameIndex = 0u;
    uint32_t enabled = 0u;
    uint32_t reverseZ = 1u;
    uint32_t cacheCapacity = kWorldGIRadianceCacheDefaultCapacity;
    uint32_t probeCount = kWorldGIRadianceCacheMaxProbeCount;
    uint32_t ttlFrames = kWorldGIRadianceCacheDefaultTTLFrames;
    float cellSizePadding[4]{0.5f, 0.0f, 0.0f, 0.0f};
  };
  static_assert(sizeof(Uniforms) == 112u, "WorldGIRadianceCacheDebugPass uniforms must match WGSL layout");

  static Settings sanitizeSettings(Settings settings)
  {
    settings.viewPortWidth = std::max(1u, settings.viewPortWidth);
    settings.viewPortHeight = std::max(1u, settings.viewPortHeight);
    settings.cacheCapacity = std::max(1u, settings.cacheCapacity);
    settings.probeCount = std::clamp(settings.probeCount, 1u, kWorldGIRadianceCacheMaxProbeCount);
    settings.ttlFrames = std::max(1u, settings.ttlFrames);
    settings.cellSize = std::max(1.0e-4f, settings.cellSize);
    return settings;
  }

  static rendering::BufferView makeBufferView(rendering::Buffer buffer, uint64_t size, rendering::AccessPattern access)
  {
    return rendering::BufferView{.buffer = buffer, .offset = 0u, .size = size, .access = access};
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

  void ensureResources()
  {
    if (layout_.isValid())
      return;

    uniformBuffer_ = renderGraph->createBuffer(
        rendering::BufferInfo{
            .name = passName + "_Uniforms.buffer",
            .size = sizeof(Uniforms),
            .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });

    layout_ = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
            .name = passName + "_Layout",
            .groups = {rendering::BindingGroupLayout{
                .buffers = {
                    {.name = "uniforms", .binding = 0u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "cacheEntries", .binding = 2u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .textures = {
                    {.name = "sceneDepthTexture", .binding = 1u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .storageTextures = {
                    {.name = "outputTexture", .binding = 3u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
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
                    {.binding = 0u, .bufferView = makeBufferView(uniformBuffer_, sizeof(Uniforms), rendering::AccessPattern::SHADER_READ)},
                    {.binding = 2u, .bufferView = makeBufferView(cacheEntryBuffer_, worldGIRadianceCacheEntryBufferSize(settings_.cacheCapacity), rendering::AccessPattern::SHADER_READ)},
                },
                .textures = {
                    {.binding = 1u, .textureView = makeDepthView(sceneDepthTexture_)},
                },
                .storageTextures = {
                    {.binding = 3u, .textureView = makeStorageColorView(outputTexture_)},
                },
            }},
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/globalIllumination/worldgi/worldgi-radiance-cache-debug-cs.spirv"),
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

  void writeUniforms()
  {
    Uniforms uniforms{};
    std::copy(std::begin(inverseViewProjection_.data), std::end(inverseViewProjection_.data), std::begin(uniforms.inverseViewProjection));
    uniforms.viewport[0] = settings_.viewPortWidth;
    uniforms.viewport[1] = settings_.viewPortHeight;
    uniforms.frameIndex = settings_.frameIndex;
    uniforms.enabled = settings_.enabled ? 1u : 0u;
    uniforms.reverseZ = settings_.reverseZ ? 1u : 0u;
    uniforms.cacheCapacity = settings_.cacheCapacity;
    uniforms.probeCount = settings_.probeCount;
    uniforms.ttlFrames = settings_.ttlFrames;
    uniforms.cellSizePadding[0] = settings_.cellSize;
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(uniforms), &uniforms);
  }

  rendering::Texture sceneDepthTexture_{};
  rendering::Buffer cacheEntryBuffer_{};
  rendering::Texture outputTexture_{};
  Settings settings_{};
  math::Mat4f inverseViewProjection_ = math::Mat4f::identity();

  rendering::BindingsLayout layout_{};
  rendering::BindingGroups bindingGroups_{};
  rendering::Shader shader_{};
  rendering::ComputePipeline pipeline_{};
  rendering::Buffer uniformBuffer_{};
};

} // namespace globalillumination::worldgi
