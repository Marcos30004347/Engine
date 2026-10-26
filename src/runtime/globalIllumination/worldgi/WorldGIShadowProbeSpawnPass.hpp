#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "globalIllumination/worldgi/WorldGITypes.hpp"
#include "math/math.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "softwarebvh/rendering/SoftwareRayTracePass.hpp"

namespace globalillumination::worldgi
{

class WorldGIShadowProbeSpawnPass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t viewPortWidth = 1920u;
    uint32_t viewPortHeight = 1080u;
    uint32_t probeGridWidth = 240u;
    uint32_t probeGridHeight = 135u;
    uint32_t frameIndex = 0u;
    bool enabled = false;
    bool historyEnabled = false;
    bool reverseZ = true;
    uint32_t tileSize = 8u;
    uint32_t raysPerProbe = kWorldGIShadowRaysPerProbe;
    uint32_t lightCount = kWorldGILightGridMaxLights;
    float minTraceDistance = 0.02f;
    float maxTraceDistance = 5000.0f;
    float normalBias = 0.03f;
  };

  WorldGIShadowProbeSpawnPass(
      rendering::Texture sceneDepthTexture,
      rendering::Texture geometryNormalTexture,
      rendering::Buffer lightBuffer,
      rendering::Buffer lightBVHNodeBuffer,
      rendering::Buffer lightBVHInfoBuffer,
      rendering::Buffer shadowProbeBuffer,
      rendering::Buffer shadowReservoirBuffer,
      rendering::Buffer shadowReservoirHistoryBuffer,
      rendering::Buffer shadowProbeDirectionalBuffer,
      rendering::Buffer shadowProbeDirectionalHistoryBuffer,
      rendering::Buffer softwareShadowRayBuffer,
      Settings settings)
      : sceneDepthTexture_(sceneDepthTexture),
        geometryNormalTexture_(geometryNormalTexture),
        lightBuffer_(lightBuffer),
        lightBVHNodeBuffer_(lightBVHNodeBuffer),
        lightBVHInfoBuffer_(lightBVHInfoBuffer),
        shadowProbeBuffer_(shadowProbeBuffer),
        shadowReservoirBuffer_(shadowReservoirBuffer),
        shadowReservoirHistoryBuffer_(shadowReservoirHistoryBuffer),
        shadowProbeDirectionalBuffer_(shadowProbeDirectionalBuffer),
        shadowProbeDirectionalHistoryBuffer_(shadowProbeDirectionalHistoryBuffer),
        softwareShadowRayBuffer_(softwareShadowRayBuffer),
        settings_(sanitizeSettings(settings))
  {
  }

  ~WorldGIShadowProbeSpawnPass() override
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
    destroyFrameOverrideBuffers();
  }

  void setSettings(Settings settings)
  {
    settings_ = sanitizeSettings(settings);
    if (uniformBuffer_.isValid())
      writeUniforms();
  }

  void updateUniforms(const math::Mat4f &inverseViewProjection, const math::Mat4f &previousViewProjection)
  {
    inverseViewProjection_ = inverseViewProjection;
    previousViewProjection_ = previousViewProjection;
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
    commandBuffer.cmdDispatch((settings_.probeGridWidth + 7u) / 8u, (settings_.probeGridHeight + 7u) / 8u, 1u);
  }

private:
  struct alignas(16) Uniforms
  {
    float inverseViewProjection[16]{};
    float previousViewProjection[16]{};
    uint32_t viewport[2]{1u, 1u};
    uint32_t probeGrid[2]{1u, 1u};
    uint32_t controls[4]{0u, 0u, 1u, 8u};
    uint32_t historyControls[4]{0u, 0u, 0u, 0u};
    float traceParams[4]{0.02f, 5000.0f, 0.03f, 0.0f};
  };
  static_assert(sizeof(Uniforms) == 192u, "WorldGIShadowProbeSpawnPass uniforms must match WGSL layout");

  static Settings sanitizeSettings(Settings settings)
  {
    settings.viewPortWidth = std::max(1u, settings.viewPortWidth);
    settings.viewPortHeight = std::max(1u, settings.viewPortHeight);
    settings.probeGridWidth = std::max(1u, settings.probeGridWidth);
    settings.probeGridHeight = std::max(1u, settings.probeGridHeight);
    settings.tileSize = std::clamp(settings.tileSize, 1u, 32u);
    settings.raysPerProbe = std::clamp(settings.raysPerProbe, 1u, kWorldGIShadowRaysPerProbe);
    settings.lightCount = std::clamp(settings.lightCount, 1u, kWorldGILightGridMaxLights);
    settings.minTraceDistance = std::max(0.0f, settings.minTraceDistance);
    settings.maxTraceDistance = std::max(settings.minTraceDistance, settings.maxTraceDistance);
    settings.normalBias = std::max(0.0f, settings.normalBias);
    return settings;
  }

  static rendering::BufferView makeBufferView(rendering::Buffer buffer, uint64_t size, rendering::AccessPattern access)
  {
    return rendering::BufferView{.buffer = buffer, .offset = 0u, .size = size, .access = access};
  }

  static rendering::TextureView makeDepthView(rendering::Texture texture)
  {
    return rendering::TextureView{.texture = texture, .access = rendering::AccessPattern::SHADER_READ, .layout = rendering::ResourceLayout::SHADER_READ_ONLY, .index = 0u, .flags = rendering::ImageAspectFlags::Depth, .baseArrayLayer = 0u, .baseMipLevel = 0u, .layerCount = 1u, .levelCount = 1u};
  }

  static rendering::TextureView makeColorView(rendering::Texture texture)
  {
    return rendering::TextureView{.texture = texture, .access = rendering::AccessPattern::SHADER_READ, .layout = rendering::ResourceLayout::SHADER_READ_ONLY, .index = 0u, .flags = rendering::ImageAspectFlags::Color, .baseArrayLayer = 0u, .baseMipLevel = 0u, .layerCount = 1u, .levelCount = 1u};
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
    uniformBuffer_ = createFrameLocalBuffer(rendering::BufferInfo{.name = passName + "_Uniforms.buffer", .size = sizeof(Uniforms), .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push});
    initializeFrameOverrideBuffers();
    layout_ = renderGraph->createBindingsLayout(rendering::BindingsLayoutInfo{
        .name = passName + "_Layout",
        .groups = {rendering::BindingGroupLayout{
            .buffers = {
                {.name = "uniforms", .binding = 0u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "worldGILights", .binding = 3u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "worldGILightBVHNodes", .binding = 4u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "worldGILightBVHInfo", .binding = 5u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "shadowProbes", .binding = 6u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "softwareShadowRays", .binding = 7u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "shadowProbeDirections", .binding = 8u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "shadowReservoirs", .binding = 9u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "shadowProbeDirectionHistory", .binding = 10u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "shadowReservoirHistory", .binding = 11u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
            },
            .textures = {
                {.name = "sceneDepthTexture", .binding = 1u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "geometryNormalTexture", .binding = 2u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
            },
        }}});
    const uint32_t probeCount = settings_.probeGridWidth * settings_.probeGridHeight;
    const uint32_t rayCount = probeCount * kWorldGIShadowRaysPerProbe;
    bindingGroups_ = renderGraph->createBindingGroups(rendering::BindingGroupsInfo{
        .layout = layout_,
        .name = passName + "_Groups",
        .groups = {rendering::GroupInfo{
            .name = "group0",
            .buffers = {
                {.binding = 0u, .bufferView = makeBufferView(uniformBuffer_, sizeof(Uniforms), rendering::AccessPattern::SHADER_READ)},
                {.binding = 3u, .bufferView = makeBufferView(lightBuffer_, worldGILightBufferSize(settings_.lightCount), rendering::AccessPattern::SHADER_READ)},
                {.binding = 4u, .bufferView = makeBufferView(lightBVHNodeBuffer_, worldGILightBVHNodeBufferSize(settings_.lightCount), rendering::AccessPattern::SHADER_READ)},
                {.binding = 5u, .bufferView = makeBufferView(lightBVHInfoBuffer_, worldGILightBVHInfoBufferSize(), rendering::AccessPattern::SHADER_READ)},
                {.binding = 6u, .bufferView = makeBufferView(shadowProbeBuffer_, worldGIShadowProbeBufferSize(probeCount), rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE)},
                {.binding = 7u, .bufferView = makeBufferView(softwareShadowRayBuffer_, static_cast<uint64_t>(rayCount) * sizeof(softwarebvh::rendering::SoftwareRayTracePass::TraceRayGPU), rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE)},
                {.binding = 8u, .bufferView = makeBufferView(shadowProbeDirectionalBuffer_, worldGIShadowProbeDirectionalBufferSize(probeCount), rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE)},
                {.binding = 9u, .bufferView = makeBufferView(shadowReservoirBuffer_, worldGIShadowReservoirBufferSize(probeCount), rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE)},
                {.binding = 10u, .bufferView = makeBufferView(shadowProbeDirectionalHistoryBuffer_, worldGIShadowProbeDirectionalBufferSize(probeCount), rendering::AccessPattern::SHADER_READ)},
                {.binding = 11u, .bufferView = makeBufferView(shadowReservoirHistoryBuffer_, worldGIShadowReservoirBufferSize(probeCount), rendering::AccessPattern::SHADER_READ)},
            },
            .textures = {
                {.binding = 1u, .textureView = makeDepthView(sceneDepthTexture_)},
                {.binding = 2u, .textureView = makeColorView(geometryNormalTexture_)},
            },
        }}});
    shader_ = renderGraph->createShader(rendering::ShaderInfo{.name = passName + "_Shader", .layout = layout_, .src = os::io::readRelativeFile("assets/shaders/spirv/globalIllumination/worldgi/worldgi-shadow-probe-spawn-cs.spirv"), .type = rendering::ShaderType::SpirV});
    pipeline_ = renderGraph->createComputePipeline(rendering::ComputePipelineInfo{.entry = "cs_main", .layout = layout_, .name = passName + "_Pipeline", .shader = shader_});
  }

  void initializeFrameOverrideBuffers()
  {
    if (renderGraph == nullptr || renderGraph->getMaxFramesInFlight() <= 1u)
      return;
    const uint32_t frameCount = renderGraph->getMaxFramesInFlight();
    frameOverrideUniformBufferIds_.assign(frameCount, rendering::BufferId::Invalid);
    for (uint32_t frameSlot = 0u; frameSlot < frameCount; ++frameSlot)
      frameOverrideUniformBufferIds_[frameSlot] = renderGraph->getRHI()->createBuffer(rendering::BufferInfo{.name = passName + "_Uniforms.override_frame" + std::to_string(frameSlot), .size = sizeof(Uniforms), .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push});
  }

  void destroyFrameOverrideBuffers()
  {
    if (renderGraph == nullptr)
      return;
    for (const rendering::BufferId bufferId : frameOverrideUniformBufferIds_)
      if (bufferId != rendering::BufferId::Invalid)
        renderGraph->getRHI()->deleteBuffer(bufferId);
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
    std::copy(std::begin(previousViewProjection_.data), std::end(previousViewProjection_.data), std::begin(uniforms.previousViewProjection));
    uniforms.viewport[0] = settings_.viewPortWidth;
    uniforms.viewport[1] = settings_.viewPortHeight;
    uniforms.probeGrid[0] = settings_.probeGridWidth;
    uniforms.probeGrid[1] = settings_.probeGridHeight;
    uniforms.controls[0] = settings_.enabled ? 1u : 0u;
    uniforms.controls[1] = settings_.frameIndex;
    uniforms.controls[2] = settings_.reverseZ ? 1u : 0u;
    uniforms.controls[3] = settings_.tileSize;
    uniforms.historyControls[0] = settings_.historyEnabled ? 1u : 0u;
    uniforms.traceParams[0] = settings_.minTraceDistance;
    uniforms.traceParams[1] = settings_.maxTraceDistance;
    uniforms.traceParams[2] = settings_.normalBias;
    uniforms.traceParams[3] = static_cast<float>(settings_.raysPerProbe);
    if (frameOverrideUniformBufferIds_.empty())
      renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(uniforms), &uniforms);
    else
      for (const rendering::BufferId bufferId : frameOverrideUniformBufferIds_)
        if (bufferId != rendering::BufferId::Invalid)
          renderGraph->getRHI()->bufferWrite(bufferId, 0u, sizeof(uniforms), &uniforms);
  }

  rendering::Texture sceneDepthTexture_{};
  rendering::Texture geometryNormalTexture_{};
  rendering::Buffer lightBuffer_{};
  rendering::Buffer lightBVHNodeBuffer_{};
  rendering::Buffer lightBVHInfoBuffer_{};
  rendering::Buffer shadowProbeBuffer_{};
  rendering::Buffer shadowReservoirBuffer_{};
  rendering::Buffer shadowReservoirHistoryBuffer_{};
  rendering::Buffer shadowProbeDirectionalBuffer_{};
  rendering::Buffer shadowProbeDirectionalHistoryBuffer_{};
  rendering::Buffer softwareShadowRayBuffer_{};
  Settings settings_{};
  math::Mat4f inverseViewProjection_ = math::Mat4f::identity();
  math::Mat4f previousViewProjection_ = math::Mat4f::identity();
  rendering::BindingsLayout layout_{};
  rendering::BindingGroups bindingGroups_{};
  rendering::Shader shader_{};
  rendering::ComputePipeline pipeline_{};
  rendering::Buffer uniformBuffer_{};
  std::vector<rendering::BufferId> frameOverrideUniformBufferIds_;
};

} // namespace globalillumination::worldgi
