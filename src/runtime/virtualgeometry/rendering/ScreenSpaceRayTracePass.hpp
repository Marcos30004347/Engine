#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "math/math.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace virtualgeometry
{
namespace gpgpu
{

class ScreenSpaceRayTracePass : public rendering::Pass
{
public:
  struct Settings
  {
    bool enabled = true;
    uint32_t width = 1920u;
    uint32_t height = 1080u;
    uint32_t hzbWidth = 0u;
    uint32_t hzbHeight = 0u;
    uint32_t hzbMipCount = 1u;
    uint32_t tileSize = 1u;
    uint32_t raysPerTile = 1u;
    uint32_t maxSteps = 64u;
    uint32_t maxFailedRays = 1u << 20u;
    float minTraceDistance = 0.01f;
    float maxTraceDistance = 3000.0f;
    float surfaceThickness = 0.0015f;
    float originBias = 0.01f;
    bool screenTraceEnabled = true;
    bool forceWorldFallback = false;
  };

  ScreenSpaceRayTracePass(
      rendering::Texture sceneDepthTexture,
      rendering::Texture geometryNormalTexture,
      rendering::Texture hzbTexture,
      Settings settings)
      : sceneDepthTexture_(sceneDepthTexture), geometryNormalTexture_(geometryNormalTexture), hzbTexture_(hzbTexture), settings_(sanitizeSettings(settings))
  {
  }

  ~ScreenSpaceRayTracePass() override
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
    if (failedRaysBuffer_.isValid())
      renderGraph->deleteBuffer(failedRaysBuffer_);
    if (failedRaysClearBuffer_.isValid())
      renderGraph->deleteBuffer(failedRaysClearBuffer_);
    if (screenHitsBuffer_.isValid())
      renderGraph->deleteBuffer(screenHitsBuffer_);
    if (screenHitsClearBuffer_.isValid())
      renderGraph->deleteBuffer(screenHitsClearBuffer_);
    if (stepCountTexture_.isValid())
      renderGraph->deleteTexture(stepCountTexture_);
  }

  void setSettings(const Settings &settings)
  {
    settings_ = sanitizeSettings(settings);
    if (uniformBuffer_.isValid())
      writeUniforms();
  }

  const Settings &getSettings() const
  {
    return settings_;
  }

  void updateUniforms(const math::Mat4f &viewProjection, const math::Mat4f &inverseViewProjection, uint32_t frameIndex, bool reverseZ)
  {
    viewProjection_ = viewProjection;
    inverseViewProjection_ = inverseViewProjection;
    frameIndex_ = frameIndex;
    reverseZ_ = reverseZ;
    if (uniformBuffer_.isValid())
      writeUniforms();
  }

  void appendFrameOverrides(rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const
  {
    if (!frameOverrideUniformBufferIds_.empty())
      overrides.bufferOverrides.emplace(uniformBuffer_.name, rendering::RenderGraphBufferOverride{.bufferId = getFrameOverrideUniformBufferId(frameSlot)});
  }

  const rendering::Texture &getStepCountTexture() const
  {
    return stepCountTexture_;
  }

  const rendering::Buffer &getFailedRaysBuffer() const
  {
    return failedRaysBuffer_;
  }

  const rendering::Buffer &getScreenHitsBuffer() const
  {
    return screenHitsBuffer_;
  }

  uint64_t getFailedRaysBufferSize() const
  {
    return failedRaysBufferSize(settings_);
  }

  uint64_t getScreenHitsBufferSize() const
  {
    return screenHitsBufferSize(settings_);
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
    writeUniforms();

    failedRaysBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_FailedRays.buffer",
            .size = failedRaysBufferSize(settings_),
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_CopySrc | rendering::BufferUsage::BufferUsage_CopyDst,
        });
    failedRaysClearBuffer_ = renderGraph->createBuffer(
        rendering::BufferInfo{
            .name = passName + "_FailedRaysClear.buffer",
            .size = sizeof(FailedRayHeaderGPU),
            .usage = rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
        });
    FailedRayHeaderGPU clearHeader{};
    clearHeader.capacity = settings_.maxFailedRays;
    renderGraph->bufferWrite(failedRaysClearBuffer_, 0u, sizeof(clearHeader), &clearHeader);

    screenHitsBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_ScreenHits.buffer",
            .size = screenHitsBufferSize(settings_),
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_CopySrc | rendering::BufferUsage::BufferUsage_CopyDst,
        });
    screenHitsClearBuffer_ = renderGraph->createBuffer(
        rendering::BufferInfo{
            .name = passName + "_ScreenHitsClear.buffer",
            .size = sizeof(ScreenHitHeaderGPU),
            .usage = rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
        });
    ScreenHitHeaderGPU clearHitHeader{};
    clearHitHeader.capacity = settings_.maxFailedRays;
    renderGraph->bufferWrite(screenHitsClearBuffer_, 0u, sizeof(clearHitHeader), &clearHitHeader);

    stepCountTexture_ = createFrameLocalTexture(
        rendering::TextureInfo{
            .name = passName + "_StepCount.texture",
            .format = rendering::Format::Format_R32Uint,
            .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
            .usage = rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
            .width = settings_.width,
            .height = settings_.height,
            .depth = 1u,
            .mipLevels = 1u,
        });

    layout_ = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
            .name = passName + "_Layout",
            .groups = {rendering::BindingGroupLayout{
                .buffers = {
                    {.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "failedRays", .binding = 4, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "screenHits", .binding = 6, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .textures = {
                    {.name = "sceneDepthTexture", .binding = 1, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "geometryNormalTexture", .binding = 2, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "hzbTexture", .binding = 3, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .storageTextures = {
                    {.name = "stepCountTexture", .binding = 5, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
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
                    {.binding = 0, .bufferView = makeReadOnlyBufferView(uniformBuffer_, sizeof(Uniforms))},
                    {.binding = 4, .bufferView = makeStorageBufferView(failedRaysBuffer_, failedRaysBufferSize(settings_))},
                    {.binding = 6, .bufferView = makeStorageBufferView(screenHitsBuffer_, screenHitsBufferSize(settings_))},
                },
                .textures = {
                    {.binding = 1, .textureView = makeSampledDepthView(sceneDepthTexture_)},
                    {.binding = 2, .textureView = makeSampledColorView(geometryNormalTexture_)},
                    {.binding = 3, .textureView = makeSampledHzbView(hzbTexture_, settings_.hzbMipCount)},
                },
                .storageTextures = {
                    {.binding = 5, .textureView = makeStorageColorView(stepCountTexture_)},
                },
            }},
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/screen-space-raytrace-cs.spirv"),
            .type = rendering::ShaderType::SpirV,
        });
    pipeline_ = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{
            .entry = "cs_main",
            .layout = layout_,
            .name = passName + "_Pipeline",
            .shader = shader_,
        });

    commandBuffer.cmdCopyBuffer(
        rendering::BufferView{
            .buffer = failedRaysClearBuffer_,
            .offset = 0u,
            .size = sizeof(FailedRayHeaderGPU),
            .access = rendering::AccessPattern::TRANSFER_READ,
        },
        rendering::BufferView{
            .buffer = failedRaysBuffer_,
            .offset = 0u,
            .size = sizeof(FailedRayHeaderGPU),
            .access = rendering::AccessPattern::TRANSFER_WRITE,
        });
    commandBuffer.cmdCopyBuffer(
        rendering::BufferView{
            .buffer = screenHitsClearBuffer_,
            .offset = 0u,
            .size = sizeof(ScreenHitHeaderGPU),
            .access = rendering::AccessPattern::TRANSFER_READ,
        },
        rendering::BufferView{
            .buffer = screenHitsBuffer_,
            .offset = 0u,
            .size = sizeof(ScreenHitHeaderGPU),
            .access = rendering::AccessPattern::TRANSFER_WRITE,
        });

    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    // Dispatch the maximum tile grid so tile size can be changed at runtime.
    const uint32_t tilesX = settings_.width;
    const uint32_t tilesY = settings_.height;
    commandBuffer.cmdDispatch((tilesX + 7u) / 8u, (tilesY + 7u) / 8u, 1u);
  }

private:
  struct Uniforms
  {
    math::Mat4f viewProjection = math::Mat4f::identity();
    math::Mat4f inverseViewProjection = math::Mat4f::identity();
    uint32_t viewport[2]{1u, 1u};
    uint32_t hzbSize[2]{1u, 1u};
    uint32_t hzbMipCount = 1u;
    uint32_t tileSize = 1u;
    uint32_t raysPerTile = 1u;
    uint32_t maxSteps = 64u;
    uint32_t maxFailedRays = 1u;
    uint32_t frameIndex = 0u;
    uint32_t reverseZ = 1u;
    uint32_t enabled = 1u;
    float minTraceDistance = 0.01f;
    float maxTraceDistance = 3000.0f;
    float surfaceThickness = 0.0015f;
    float originBias = 0.01f;
    uint32_t forceWorldFallback = 0u;
    uint32_t screenTraceEnabled = 1u;
    uint32_t _padding0[2] = {};
  };

public:
  struct FailedRayGPU
  {
    float origin[4];
    float direction[4];
    uint32_t pixelAndRay[4];
  };

  struct FailedRayHeaderGPU
  {
    uint32_t count = 0u;
    uint32_t overflow = 0u;
    uint32_t capacity = 0u;
    uint32_t totalRays = 0u;
    uint32_t offscreenRays = 0u;
    uint32_t _padding[3] = {};
  };

  struct ScreenHitGPU
  {
    float origin[4];
    float direction[4];
    float hitPositionDistance[4];
    uint32_t pixelsAndRay[4];
  };

  struct ScreenHitHeaderGPU
  {
    uint32_t count = 0u;
    uint32_t overflow = 0u;
    uint32_t capacity = 0u;
    uint32_t totalHits = 0u;
    uint32_t _padding[4] = {};
  };

  static_assert(sizeof(Uniforms) == 208u, "ScreenSpaceRayTracePass::Uniforms must stay tightly packed");
  static_assert(sizeof(FailedRayGPU) == 48u, "ScreenSpaceRayTracePass::FailedRayGPU must stay tightly packed");
  static_assert(sizeof(FailedRayHeaderGPU) == 32u, "ScreenSpaceRayTracePass::FailedRayHeaderGPU must stay tightly packed");
  static_assert(sizeof(ScreenHitGPU) == 64u, "ScreenSpaceRayTracePass::ScreenHitGPU must stay tightly packed");
  static_assert(sizeof(ScreenHitHeaderGPU) == 32u, "ScreenSpaceRayTracePass::ScreenHitHeaderGPU must stay tightly packed");

private:
  static Settings sanitizeSettings(Settings settings)
  {
    settings.width = std::max(1u, settings.width);
    settings.height = std::max(1u, settings.height);
    settings.hzbWidth = settings.hzbWidth == 0u ? settings.width : settings.hzbWidth;
    settings.hzbHeight = settings.hzbHeight == 0u ? settings.height : settings.hzbHeight;
    settings.hzbMipCount = std::max(1u, settings.hzbMipCount);
    settings.maxSteps = std::max(1u, settings.maxSteps);
    settings.raysPerTile = std::clamp(settings.raysPerTile, 1u, 1024u);
    settings.maxFailedRays = std::max(1u, settings.maxFailedRays);
    settings.tileSize = isSupportedTileSize(settings.tileSize) ? settings.tileSize : 1u;
    settings.minTraceDistance = std::max(0.0f, settings.minTraceDistance);
    settings.maxTraceDistance = std::max(settings.maxTraceDistance, settings.minTraceDistance + 1.0e-4f);
    settings.surfaceThickness = std::max(0.0f, settings.surfaceThickness);
    settings.originBias = std::max(0.0f, settings.originBias);
    return settings;
  }

  static bool isSupportedTileSize(uint32_t tileSize)
  {
    return tileSize == 1u || tileSize == 2u || tileSize == 4u || tileSize == 8u || tileSize == 16u;
  }

  static uint64_t failedRaysBufferSize(const Settings &settings)
  {
    return sizeof(FailedRayHeaderGPU) + static_cast<uint64_t>(std::max(1u, settings.maxFailedRays)) * sizeof(FailedRayGPU);
  }

public:
  static uint64_t screenHitsBufferSizeForCapacity(uint32_t capacity)
  {
    return sizeof(ScreenHitHeaderGPU) + static_cast<uint64_t>(std::max(1u, capacity)) * sizeof(ScreenHitGPU);
  }

private:
  static uint64_t screenHitsBufferSize(const Settings &settings)
  {
    return screenHitsBufferSizeForCapacity(settings.maxFailedRays);
  }

  static rendering::BufferView makeReadOnlyBufferView(rendering::Buffer buffer, uint64_t size)
  {
    return rendering::BufferView{.buffer = buffer, .offset = 0u, .size = size, .access = rendering::AccessPattern::SHADER_READ};
  }

  static rendering::BufferView makeStorageBufferView(rendering::Buffer buffer, uint64_t size)
  {
    return rendering::BufferView{.buffer = buffer, .offset = 0u, .size = size, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE};
  }

  static rendering::TextureView makeSampledColorView(rendering::Texture texture)
  {
    return rendering::TextureView{
        .texture = texture,
        .access = rendering::AccessPattern::SHADER_READ,
        .layout = rendering::ResourceLayout::SHADER_READ_ONLY,
        .index = 0,
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
        .index = 0,
        .flags = rendering::ImageAspectFlags::Depth,
        .baseArrayLayer = 0u,
        .baseMipLevel = 0u,
        .layerCount = 1u,
        .levelCount = 1u,
    };
  }

  static rendering::TextureView makeSampledHzbView(rendering::Texture texture, uint32_t mipCount)
  {
    return rendering::TextureView{
        .texture = texture,
        .access = rendering::AccessPattern::SHADER_READ,
        .layout = rendering::ResourceLayout::SHADER_READ_ONLY,
        .index = 0,
        .flags = rendering::ImageAspectFlags::Color,
        .baseArrayLayer = 0u,
        .baseMipLevel = 0u,
        .layerCount = 1u,
        .levelCount = mipCount,
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
        .baseArrayLayer = 0u,
        .baseMipLevel = 0u,
        .layerCount = 1u,
        .levelCount = 1u,
    };
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

  rendering::BufferId getCurrentFrameOverrideUniformBufferId() const
  {
    return getFrameOverrideUniformBufferId(renderGraph == nullptr ? 0u : renderGraph->getCurrentFrameIndex());
  }

  void writeUniforms()
  {
    Uniforms uniforms{};
    uniforms.viewProjection = viewProjection_;
    uniforms.inverseViewProjection = inverseViewProjection_;
    uniforms.viewport[0] = settings_.width;
    uniforms.viewport[1] = settings_.height;
    uniforms.hzbSize[0] = settings_.hzbWidth;
    uniforms.hzbSize[1] = settings_.hzbHeight;
    uniforms.hzbMipCount = settings_.hzbMipCount;
    uniforms.tileSize = settings_.tileSize;
    uniforms.raysPerTile = settings_.raysPerTile;
    uniforms.maxSteps = settings_.maxSteps;
    uniforms.maxFailedRays = settings_.maxFailedRays;
    uniforms.frameIndex = frameIndex_;
    uniforms.reverseZ = reverseZ_ ? 1u : 0u;
    uniforms.enabled = settings_.enabled ? 1u : 0u;
    uniforms.minTraceDistance = settings_.minTraceDistance;
    uniforms.maxTraceDistance = settings_.maxTraceDistance;
    uniforms.surfaceThickness = settings_.surfaceThickness;
    uniforms.originBias = settings_.originBias;
    uniforms.forceWorldFallback = settings_.forceWorldFallback || !settings_.screenTraceEnabled ? 1u : 0u;
    uniforms.screenTraceEnabled = settings_.screenTraceEnabled ? 1u : 0u;

    if (frameOverrideUniformBufferIds_.empty())
    {
      renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(Uniforms), &uniforms);
    }
    else
    {
      renderGraph->getRHI()->bufferWrite(getCurrentFrameOverrideUniformBufferId(), 0u, sizeof(Uniforms), &uniforms);
    }
  }

  rendering::Texture sceneDepthTexture_{};
  rendering::Texture geometryNormalTexture_{};
  rendering::Texture hzbTexture_{};
  Settings settings_{};
  math::Mat4f viewProjection_ = math::Mat4f::identity();
  math::Mat4f inverseViewProjection_ = math::Mat4f::identity();
  uint32_t frameIndex_ = 0u;
  bool reverseZ_ = true;

  rendering::BindingsLayout layout_{};
  rendering::BindingGroups bindingGroups_{};
  rendering::Shader shader_{};
  rendering::ComputePipeline pipeline_{};
  rendering::Buffer uniformBuffer_{};
  rendering::Buffer failedRaysBuffer_{};
  rendering::Buffer failedRaysClearBuffer_{};
  rendering::Buffer screenHitsBuffer_{};
  rendering::Buffer screenHitsClearBuffer_{};
  rendering::Texture stepCountTexture_{};
  std::vector<rendering::BufferId> frameOverrideUniformBufferIds_{};
};

} // namespace gpgpu
} // namespace virtualgeometry
