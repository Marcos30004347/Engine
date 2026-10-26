#pragma once

#include <algorithm>
#include <cstdint>

#include "globalIllumination/worldgi/WorldGITypes.hpp"
#include "math/math.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace globalillumination::worldgi
{

class WorldGIRadianceCacheShadePass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t activeCapacity = 0u;
    uint32_t frameIndex = 0u;
    bool enabled = true;
    bool lightGridEnabled = true;
    bool lightBVHEnabled = true;
    uint32_t lightGridResolution = kWorldGILightGridDefaultResolution;
    uint32_t lightGridCascadeCount = 6u;
    uint32_t lightGridMaxItemsPerCell = kWorldGILightGridDefaultMaxItemsPerCell;
    uint32_t lightCount = 2u;
    uint32_t cacheCapacity = kWorldGIRadianceCacheDefaultCapacity;
    math::Vec3f lightGridCenter = math::Vec3f(0.0f);
    float lightGridBaseExtent = 2000.0f;
    float lightGridCascadeScale = 2.0f;
    float clampValue = 32.0f;
    float hysteresis = 0.0f;
  };

  WorldGIRadianceCacheShadePass(
      rendering::Buffer cacheEntryBuffer,
      rendering::Buffer activeListBuffer,
      rendering::Buffer counterBuffer,
      rendering::Buffer traceSurfaceBuffer,
      rendering::Buffer lightBuffer,
      rendering::Buffer lightBVHNodeBuffer,
      rendering::Buffer lightBVHInfoBuffer,
      rendering::Buffer lightGridCellBuffer,
      rendering::Buffer lightGridItemIdBuffer,
      Settings settings)
      : cacheEntryBuffer_(cacheEntryBuffer),
        activeListBuffer_(activeListBuffer),
        counterBuffer_(counterBuffer),
        traceSurfaceBuffer_(traceSurfaceBuffer),
        lightBuffer_(lightBuffer),
        lightBVHNodeBuffer_(lightBVHNodeBuffer),
        lightBVHInfoBuffer_(lightBVHInfoBuffer),
        lightGridCellBuffer_(lightGridCellBuffer),
        lightGridItemIdBuffer_(lightGridItemIdBuffer),
        settings_(sanitizeSettings(settings))
  {
  }

  ~WorldGIRadianceCacheShadePass() override
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
    if (settings_.activeCapacity == 0u)
      return;

    ensureResources();
    writeUniforms();

    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    commandBuffer.cmdDispatch((settings_.activeCapacity + 63u) / 64u, 1u, 1u);
  }

private:
  struct alignas(16) Uniforms
  {
    uint32_t activeCapacity = 0u;
    uint32_t frameIndex = 0u;
    uint32_t enabled = 1u;
    uint32_t lightGridEnabled = 1u;
    uint32_t lightGridResolution[3]{kWorldGILightGridDefaultResolution, kWorldGILightGridDefaultResolution, kWorldGILightGridDefaultResolution};
    uint32_t lightGridCascadeCount = 6u;
    uint32_t lightGridMaxItemsPerCell = kWorldGILightGridDefaultMaxItemsPerCell;
    uint32_t lightCount = 2u;
    uint32_t cacheCapacity = kWorldGIRadianceCacheDefaultCapacity;
    uint32_t lightBVHEnabled = 1u;
    float lightGridCenterBaseExtent[4]{0.0f, 0.0f, 0.0f, 2000.0f};
    float lightGridCascadeScalePadding[4]{2.0f, 0.0f, 0.0f, 0.0f};
    float clampHysteresisPadding[4]{32.0f, 0.0f, 0.0f, 0.0f};
  };
  static_assert(sizeof(Uniforms) == 96u, "WorldGIRadianceCacheShadePass uniforms must match WGSL layout");

  static Settings sanitizeSettings(Settings settings)
  {
    settings.lightGridResolution = std::clamp(settings.lightGridResolution, 1u, 64u);
    settings.lightGridCascadeCount = std::clamp(settings.lightGridCascadeCount, 1u, kWorldGILightGridMaxCascades);
    settings.lightGridMaxItemsPerCell = std::clamp(settings.lightGridMaxItemsPerCell, 1u, 256u);
    settings.lightCount = std::clamp(settings.lightCount, 1u, kWorldGILightGridMaxLights);
    settings.cacheCapacity = std::max(1u, settings.cacheCapacity);
    settings.lightGridBaseExtent = std::max(0.001f, settings.lightGridBaseExtent);
    settings.lightGridCascadeScale = std::max(1.0f, settings.lightGridCascadeScale);
    settings.clampValue = std::max(0.0f, settings.clampValue);
    settings.hysteresis = std::clamp(settings.hysteresis, 0.0f, 0.99f);
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
                    {.name = "cacheEntries", .binding = 1u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "activeList", .binding = 2u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "counters", .binding = 3u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "traceSurfaces", .binding = 4u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "worldGILights", .binding = 5u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "worldGILightGridCells", .binding = 6u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "worldGILightGridItemIds", .binding = 7u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "worldGILightBVHNodes", .binding = 8u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "worldGILightBVHInfo", .binding = 9u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
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
                    {.binding = 1u, .bufferView = makeBufferView(cacheEntryBuffer_, worldGIRadianceCacheEntryBufferSize(settings_.cacheCapacity), rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE)},
                    {.binding = 2u, .bufferView = makeBufferView(activeListBuffer_, worldGIRadianceCacheActiveListBufferSize(settings_.activeCapacity), rendering::AccessPattern::SHADER_READ)},
                    {.binding = 3u, .bufferView = makeBufferView(counterBuffer_, worldGIRadianceCacheCounterBufferSize(), rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE)},
                    {.binding = 4u, .bufferView = makeBufferView(traceSurfaceBuffer_, giTraceSurfaceBufferSize(settings_.activeCapacity), rendering::AccessPattern::SHADER_READ)},
                    {.binding = 5u, .bufferView = makeBufferView(lightBuffer_, worldGILightBufferSize(settings_.lightCount), rendering::AccessPattern::SHADER_READ)},
                    {.binding = 6u, .bufferView = makeBufferView(lightGridCellBuffer_, worldGILightGridCellBufferSize(settings_.lightGridResolution, settings_.lightGridCascadeCount), rendering::AccessPattern::SHADER_READ)},
                    {.binding = 7u, .bufferView = makeBufferView(lightGridItemIdBuffer_, worldGILightGridItemBufferSize(settings_.lightGridResolution, settings_.lightGridCascadeCount, settings_.lightGridMaxItemsPerCell), rendering::AccessPattern::SHADER_READ)},
                    {.binding = 8u, .bufferView = makeBufferView(lightBVHNodeBuffer_, worldGILightBVHNodeBufferSize(settings_.lightCount), rendering::AccessPattern::SHADER_READ)},
                    {.binding = 9u, .bufferView = makeBufferView(lightBVHInfoBuffer_, worldGILightBVHInfoBufferSize(), rendering::AccessPattern::SHADER_READ)},
                },
            }},
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/globalIllumination/worldgi/worldgi-radiance-cache-shade-cs.spirv"),
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
    uniforms.activeCapacity = settings_.activeCapacity;
    uniforms.frameIndex = settings_.frameIndex;
    uniforms.enabled = settings_.enabled ? 1u : 0u;
    uniforms.lightGridEnabled = settings_.lightGridEnabled ? 1u : 0u;
    uniforms.lightGridResolution[0] = settings_.lightGridResolution;
    uniforms.lightGridResolution[1] = settings_.lightGridResolution;
    uniforms.lightGridResolution[2] = settings_.lightGridResolution;
    uniforms.lightGridCascadeCount = settings_.lightGridCascadeCount;
    uniforms.lightGridMaxItemsPerCell = settings_.lightGridMaxItemsPerCell;
    uniforms.lightCount = settings_.lightCount;
    uniforms.cacheCapacity = settings_.cacheCapacity;
    uniforms.lightBVHEnabled = settings_.lightBVHEnabled ? 1u : 0u;
    uniforms.lightGridCenterBaseExtent[0] = settings_.lightGridCenter.x();
    uniforms.lightGridCenterBaseExtent[1] = settings_.lightGridCenter.y();
    uniforms.lightGridCenterBaseExtent[2] = settings_.lightGridCenter.z();
    uniforms.lightGridCenterBaseExtent[3] = settings_.lightGridBaseExtent;
    uniforms.lightGridCascadeScalePadding[0] = settings_.lightGridCascadeScale;
    uniforms.clampHysteresisPadding[0] = settings_.clampValue;
    uniforms.clampHysteresisPadding[1] = settings_.hysteresis;
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(uniforms), &uniforms);
  }

  rendering::Buffer cacheEntryBuffer_{};
  rendering::Buffer activeListBuffer_{};
  rendering::Buffer counterBuffer_{};
  rendering::Buffer traceSurfaceBuffer_{};
  rendering::Buffer lightBuffer_{};
  rendering::Buffer lightBVHNodeBuffer_{};
  rendering::Buffer lightBVHInfoBuffer_{};
  rendering::Buffer lightGridCellBuffer_{};
  rendering::Buffer lightGridItemIdBuffer_{};
  Settings settings_{};

  rendering::BindingsLayout layout_{};
  rendering::BindingGroups bindingGroups_{};
  rendering::Shader shader_{};
  rendering::ComputePipeline pipeline_{};
  rendering::Buffer uniformBuffer_{};
};

} // namespace globalillumination::worldgi
