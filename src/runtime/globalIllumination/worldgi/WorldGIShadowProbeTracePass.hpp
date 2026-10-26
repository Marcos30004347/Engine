#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "globalIllumination/raytracing/ScreenRayTracePass.hpp"
#include "globalIllumination/worldgi/WorldGITypes.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "softwarebvh/rendering/SoftwareRayTracePass.hpp"

namespace globalillumination::worldgi
{

class WorldGIShadowProbeTracePass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t probeGridWidth = 240u;
    uint32_t probeGridHeight = 135u;
    uint32_t raysPerProbe = kWorldGIShadowRaysPerProbe;
    uint32_t frameIndex = 0u;
    bool enabled = false;
  };

  WorldGIShadowProbeTracePass(rendering::Buffer shadowProbeBuffer, rendering::Buffer shadowReservoirBuffer, rendering::Buffer shadowProbeDirectionalBuffer, rendering::Buffer softwareShadowRayBuffer, rendering::Buffer softwareShadowHitBuffer, rendering::Buffer screenShadowHitBuffer, Settings settings)
      : shadowProbeBuffer_(shadowProbeBuffer), shadowReservoirBuffer_(shadowReservoirBuffer), shadowProbeDirectionalBuffer_(shadowProbeDirectionalBuffer), softwareShadowRayBuffer_(softwareShadowRayBuffer), softwareShadowHitBuffer_(softwareShadowHitBuffer), screenShadowHitBuffer_(screenShadowHitBuffer), settings_(sanitizeSettings(settings))
  {
  }

  ~WorldGIShadowProbeTracePass() override
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
    const uint32_t probeCount = settings_.probeGridWidth * settings_.probeGridHeight;
    const uint32_t rayCount = probeCount * kWorldGIShadowRaysPerProbe;
    commandBuffer.cmdDispatch((rayCount + 63u) / 64u, 1u, 1u);
  }

private:
  struct alignas(16) Uniforms
  {
    uint32_t probeGrid[2]{1u, 1u};
    uint32_t controls[2]{0u, kWorldGIShadowRaysPerProbe};
    uint32_t frameIndex = 0u;
    uint32_t _padding[3]{0u, 0u, 0u};
  };
  static_assert(sizeof(Uniforms) == 32u, "WorldGIShadowProbeTracePass uniforms must match WGSL layout");

  static Settings sanitizeSettings(Settings settings)
  {
    settings.probeGridWidth = std::max(1u, settings.probeGridWidth);
    settings.probeGridHeight = std::max(1u, settings.probeGridHeight);
    settings.raysPerProbe = std::clamp(settings.raysPerProbe, 1u, kWorldGIShadowRaysPerProbe);
    return settings;
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
    uniformBuffer_ = createFrameLocalBuffer(rendering::BufferInfo{.name = passName + "_Uniforms.buffer", .size = sizeof(Uniforms), .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push});
    initializeFrameOverrideBuffers();
    layout_ = renderGraph->createBindingsLayout(rendering::BindingsLayoutInfo{
        .name = passName + "_Layout",
        .groups = {rendering::BindingGroupLayout{
            .buffers = {
                {.name = "uniforms", .binding = 0u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "shadowProbes", .binding = 1u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "softwareShadowRays", .binding = 2u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "softwareShadowHits", .binding = 3u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "shadowProbeDirections", .binding = 4u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "shadowReservoirs", .binding = 5u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "screenShadowHits", .binding = 6u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
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
                {.binding = 1u, .bufferView = makeBufferView(shadowProbeBuffer_, worldGIShadowProbeBufferSize(probeCount), rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE)},
                {.binding = 2u, .bufferView = makeBufferView(softwareShadowRayBuffer_, static_cast<uint64_t>(rayCount) * sizeof(softwarebvh::rendering::SoftwareRayTracePass::TraceRayGPU), rendering::AccessPattern::SHADER_READ)},
                {.binding = 3u, .bufferView = makeBufferView(softwareShadowHitBuffer_, static_cast<uint64_t>(rayCount) * sizeof(softwarebvh::rendering::SoftwareRayTracePass::HitOutputGPU), rendering::AccessPattern::SHADER_READ)},
                {.binding = 4u, .bufferView = makeBufferView(shadowProbeDirectionalBuffer_, worldGIShadowProbeDirectionalBufferSize(probeCount), rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE)},
                {.binding = 5u, .bufferView = makeBufferView(shadowReservoirBuffer_, worldGIShadowReservoirBufferSize(probeCount), rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE)},
                {.binding = 6u, .bufferView = makeBufferView(screenShadowHitBuffer_, raytracing::ScreenRayTracePass::screenHitBufferSize(rayCount), rendering::AccessPattern::SHADER_READ)},
            },
        }}});
    shader_ = renderGraph->createShader(rendering::ShaderInfo{.name = passName + "_Shader", .layout = layout_, .src = os::io::readRelativeFile("assets/shaders/spirv/globalIllumination/worldgi/worldgi-shadow-probe-trace-cs.spirv"), .type = rendering::ShaderType::SpirV});
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
    uniforms.probeGrid[0] = settings_.probeGridWidth;
    uniforms.probeGrid[1] = settings_.probeGridHeight;
    uniforms.controls[0] = settings_.enabled ? 1u : 0u;
    uniforms.controls[1] = settings_.raysPerProbe;
    uniforms.frameIndex = settings_.frameIndex;
    if (frameOverrideUniformBufferIds_.empty())
      renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(uniforms), &uniforms);
    else
      for (const rendering::BufferId bufferId : frameOverrideUniformBufferIds_)
        if (bufferId != rendering::BufferId::Invalid)
          renderGraph->getRHI()->bufferWrite(bufferId, 0u, sizeof(uniforms), &uniforms);
  }

  rendering::Buffer shadowProbeBuffer_{};
  rendering::Buffer shadowReservoirBuffer_{};
  rendering::Buffer shadowProbeDirectionalBuffer_{};
  rendering::Buffer softwareShadowRayBuffer_{};
  rendering::Buffer softwareShadowHitBuffer_{};
  rendering::Buffer screenShadowHitBuffer_{};
  Settings settings_{};
  rendering::BindingsLayout layout_{};
  rendering::BindingGroups bindingGroups_{};
  rendering::Shader shader_{};
  rendering::ComputePipeline pipeline_{};
  rendering::Buffer uniformBuffer_{};
  std::vector<rendering::BufferId> frameOverrideUniformBufferIds_;
};

} // namespace globalillumination::worldgi
