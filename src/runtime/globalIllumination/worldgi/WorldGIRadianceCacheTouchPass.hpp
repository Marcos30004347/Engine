#pragma once

#include <algorithm>
#include <cstdint>

#include "globalIllumination/worldgi/WorldGITypes.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace globalillumination::worldgi
{

class WorldGIRadianceCacheTouchPass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t surfaceCount = 0u;
    uint32_t frameIndex = 0u;
    bool enabled = true;
    bool screenHitsEnabled = false;
    uint32_t cacheCapacity = kWorldGIRadianceCacheDefaultCapacity;
    uint32_t probeCount = kWorldGIRadianceCacheMaxProbeCount;
    uint32_t ttlFrames = kWorldGIRadianceCacheDefaultTTLFrames;
    float cellSize = 0.5f;
    float minConfidence = 0.001f;
  };

  WorldGIRadianceCacheTouchPass(
      rendering::Buffer traceSurfaceBuffer,
      rendering::Buffer cacheEntryBuffer,
      rendering::Buffer activeListBuffer,
      rendering::Buffer counterBuffer,
      Settings settings)
      : traceSurfaceBuffer_(traceSurfaceBuffer),
        cacheEntryBuffer_(cacheEntryBuffer),
        activeListBuffer_(activeListBuffer),
        counterBuffer_(counterBuffer),
        settings_(sanitizeSettings(settings))
  {
  }

  ~WorldGIRadianceCacheTouchPass() override
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

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    if (settings_.surfaceCount == 0u)
      return;

    ensureResources();
    writeUniforms();
    clearCounters();

    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    commandBuffer.cmdDispatch((settings_.surfaceCount + 63u) / 64u, 1u, 1u);
  }

private:
  struct alignas(16) Uniforms
  {
    uint32_t surfaceCount = 0u;
    uint32_t frameIndex = 0u;
    uint32_t enabled = 1u;
    uint32_t screenHitsEnabled = 0u;
    uint32_t cacheCapacity = kWorldGIRadianceCacheDefaultCapacity;
    uint32_t probeCount = kWorldGIRadianceCacheMaxProbeCount;
    uint32_t ttlFrames = kWorldGIRadianceCacheDefaultTTLFrames;
    uint32_t _padding0 = 0u;
    float cellSizeMinConfidence[4]{0.5f, 0.001f, 0.0f, 0.0f};
  };
  static_assert(sizeof(Uniforms) == 48u, "WorldGIRadianceCacheTouchPass uniforms must match WGSL layout");

  static Settings sanitizeSettings(Settings settings)
  {
    settings.cacheCapacity = std::max(1u, settings.cacheCapacity);
    settings.probeCount = std::clamp(settings.probeCount, 1u, kWorldGIRadianceCacheMaxProbeCount);
    settings.ttlFrames = std::max(1u, settings.ttlFrames);
    settings.cellSize = std::max(1.0e-4f, settings.cellSize);
    settings.minConfidence = std::max(0.0f, settings.minConfidence);
    return settings;
  }

  static rendering::BufferView makeBufferView(rendering::Buffer buffer, uint64_t size, rendering::AccessPattern access)
  {
    return rendering::BufferView{.buffer = buffer, .offset = 0u, .size = size, .access = access};
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
                    {.name = "traceSurfaces", .binding = 1u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "cacheEntries", .binding = 2u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "activeList", .binding = 3u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "counters", .binding = 4u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
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
                    {.binding = 1u, .bufferView = makeBufferView(traceSurfaceBuffer_, giTraceSurfaceBufferSize(settings_.surfaceCount), rendering::AccessPattern::SHADER_READ)},
                    {.binding = 2u, .bufferView = makeBufferView(cacheEntryBuffer_, worldGIRadianceCacheEntryBufferSize(settings_.cacheCapacity), rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE)},
                    {.binding = 3u, .bufferView = makeBufferView(activeListBuffer_, worldGIRadianceCacheActiveListBufferSize(settings_.surfaceCount), rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE)},
                    {.binding = 4u, .bufferView = makeBufferView(counterBuffer_, worldGIRadianceCacheCounterBufferSize(), rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE)},
                },
            }},
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/globalIllumination/worldgi/worldgi-radiance-cache-touch-cs.spirv"),
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
    uniforms.surfaceCount = settings_.surfaceCount;
    uniforms.frameIndex = settings_.frameIndex;
    uniforms.enabled = settings_.enabled ? 1u : 0u;
    uniforms.screenHitsEnabled = settings_.screenHitsEnabled ? 1u : 0u;
    uniforms.cacheCapacity = settings_.cacheCapacity;
    uniforms.probeCount = settings_.probeCount;
    uniforms.ttlFrames = settings_.ttlFrames;
    uniforms.cellSizeMinConfidence[0] = settings_.cellSize;
    uniforms.cellSizeMinConfidence[1] = settings_.minConfidence;
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(uniforms), &uniforms);
  }

  void clearCounters()
  {
    const uint32_t zeroCounters[4]{0u, 0u, 0u, 0u};
    renderGraph->bufferWrite(counterBuffer_, 0u, sizeof(zeroCounters), const_cast<uint32_t *>(zeroCounters));
  }

  rendering::Buffer traceSurfaceBuffer_{};
  rendering::Buffer cacheEntryBuffer_{};
  rendering::Buffer activeListBuffer_{};
  rendering::Buffer counterBuffer_{};
  Settings settings_{};

  rendering::BindingsLayout layout_{};
  rendering::BindingGroups bindingGroups_{};
  rendering::Shader shader_{};
  rendering::ComputePipeline pipeline_{};
  rendering::Buffer uniformBuffer_{};
};

} // namespace globalillumination::worldgi
