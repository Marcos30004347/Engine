#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "math/math.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "softwarebvh/rendering/SoftwareRayTracePass.hpp"

namespace globalillumination::raytracing
{

enum class RayTracePurpose : uint32_t
{
  Shadow = 0u,
  Occlusion = 1u,
  Shading = 2u,
};

class ScreenRayTracePass : public rendering::Pass
{
public:
  struct ScreenHitGPU
  {
    uint32_t metadata[4]{0u, 0u, 0u, 0u};
    float hit[4]{0.0f, 0.0f, 0.0f, 0.0f};
  };
  static_assert(sizeof(ScreenHitGPU) == 32u, "ScreenRayTracePass::ScreenHitGPU must match WGSL layout");

  struct Settings
  {
    uint32_t viewPortWidth = 1920u;
    uint32_t viewPortHeight = 1080u;
    uint32_t hzbWidth = 0u;
    uint32_t hzbHeight = 0u;
    uint32_t hzbMipCount = 1u;
    uint32_t rayCount = 1u;
    uint32_t maxSteps = 64u;
    RayTracePurpose purpose = RayTracePurpose::Shadow;
    bool enabled = false;
    bool reverseZ = true;
    bool invalidateScreenHitRays = true;
    float surfaceThickness = 0.00015f;
    float maxTraceDistance = 5000.0f;
  };

  ScreenRayTracePass(
      rendering::Texture sceneDepthTexture,
      rendering::Texture hzbTexture,
      rendering::Buffer rayBuffer,
      rendering::Buffer screenHitBuffer,
      Settings settings)
      : sceneDepthTexture_(sceneDepthTexture),
        hzbTexture_(hzbTexture),
        rayBuffer_(rayBuffer),
        screenHitBuffer_(screenHitBuffer),
        settings_(sanitizeSettings(settings))
  {
  }

  ~ScreenRayTracePass() override
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

  static constexpr uint64_t screenHitBufferSize(uint32_t rayCount)
  {
    return static_cast<uint64_t>(rayCount == 0u ? 1u : rayCount) * sizeof(ScreenHitGPU);
  }

  void setSettings(Settings settings)
  {
    settings_ = sanitizeSettings(settings);
    if (uniformBuffer_.isValid())
      writeUniforms();
  }

  void updateUniforms(const math::Mat4f &viewProjection)
  {
    viewProjection_ = viewProjection;
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
    commandBuffer.cmdDispatch((settings_.rayCount + 63u) / 64u, 1u, 1u);
  }

private:
  struct alignas(16) Uniforms
  {
    float viewProjection[16]{};
    uint32_t viewport[2]{1u, 1u};
    uint32_t hzbSize[2]{1u, 1u};
    uint32_t controls[4]{0u, 1u, 1u, 64u};
    uint32_t dispatchParams[4]{1u, 0u, 0u, 0u};
    uint32_t modeParams[4]{0u, 1u, 0u, 0u};
    float traceParams[4]{0.00015f, 5000.0f, 0.0f, 0.0f};
  };
  static_assert(sizeof(Uniforms) == 144u, "ScreenRayTracePass uniforms must match WGSL layout");

  static Settings sanitizeSettings(Settings settings)
  {
    settings.viewPortWidth = std::max(1u, settings.viewPortWidth);
    settings.viewPortHeight = std::max(1u, settings.viewPortHeight);
    settings.hzbWidth = std::max(1u, settings.hzbWidth);
    settings.hzbHeight = std::max(1u, settings.hzbHeight);
    settings.hzbMipCount = std::max(1u, settings.hzbMipCount);
    settings.rayCount = std::max(1u, settings.rayCount);
    settings.maxSteps = std::max(1u, settings.maxSteps);
    settings.surfaceThickness = std::max(0.0f, settings.surfaceThickness);
    settings.maxTraceDistance = std::max(0.0f, settings.maxTraceDistance);
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

  rendering::TextureView makeHzbView(rendering::Texture texture) const
  {
    return rendering::TextureView{.texture = texture, .access = rendering::AccessPattern::SHADER_READ, .layout = rendering::ResourceLayout::SHADER_READ_ONLY, .index = 0u, .flags = rendering::ImageAspectFlags::Color, .baseArrayLayer = 0u, .baseMipLevel = 0u, .layerCount = 1u, .levelCount = settings_.hzbMipCount};
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
                {.name = "rays", .binding = 3u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "screenHits", .binding = 4u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
            },
            .textures = {
                {.name = "sceneDepthTexture", .binding = 1u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "hzbTexture", .binding = 2u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
            },
        }}});
    bindingGroups_ = renderGraph->createBindingGroups(rendering::BindingGroupsInfo{
        .layout = layout_,
        .name = passName + "_Groups",
        .groups = {rendering::GroupInfo{
            .name = "group0",
            .buffers = {
                {.binding = 0u, .bufferView = makeBufferView(uniformBuffer_, sizeof(Uniforms), rendering::AccessPattern::SHADER_READ)},
                {.binding = 3u, .bufferView = makeBufferView(rayBuffer_, static_cast<uint64_t>(settings_.rayCount) * sizeof(softwarebvh::rendering::SoftwareRayTracePass::TraceRayGPU), rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE)},
                {.binding = 4u, .bufferView = makeBufferView(screenHitBuffer_, screenHitBufferSize(settings_.rayCount), rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE)},
            },
            .textures = {
                {.binding = 1u, .textureView = makeDepthView(sceneDepthTexture_)},
                {.binding = 2u, .textureView = makeHzbView(hzbTexture_)},
            },
        }}});
    shader_ = renderGraph->createShader(rendering::ShaderInfo{.name = passName + "_Shader", .layout = layout_, .src = os::io::readRelativeFile("assets/shaders/spirv/globalIllumination/raytracing/screen-raytrace-cs.spirv"), .type = rendering::ShaderType::SpirV});
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
    std::copy(std::begin(viewProjection_.data), std::end(viewProjection_.data), std::begin(uniforms.viewProjection));
    uniforms.viewport[0] = settings_.viewPortWidth;
    uniforms.viewport[1] = settings_.viewPortHeight;
    uniforms.hzbSize[0] = settings_.hzbWidth;
    uniforms.hzbSize[1] = settings_.hzbHeight;
    uniforms.controls[0] = settings_.enabled ? 1u : 0u;
    uniforms.controls[1] = settings_.reverseZ ? 1u : 0u;
    uniforms.controls[2] = settings_.hzbMipCount;
    uniforms.controls[3] = settings_.maxSteps;
    uniforms.dispatchParams[0] = settings_.rayCount;
    uniforms.modeParams[0] = static_cast<uint32_t>(settings_.purpose);
    uniforms.modeParams[1] = settings_.invalidateScreenHitRays ? 1u : 0u;
    uniforms.traceParams[0] = settings_.surfaceThickness;
    uniforms.traceParams[1] = settings_.maxTraceDistance;
    if (frameOverrideUniformBufferIds_.empty())
      renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(uniforms), &uniforms);
    else
      for (const rendering::BufferId bufferId : frameOverrideUniformBufferIds_)
        if (bufferId != rendering::BufferId::Invalid)
          renderGraph->getRHI()->bufferWrite(bufferId, 0u, sizeof(uniforms), &uniforms);
  }

  rendering::Texture sceneDepthTexture_{};
  rendering::Texture hzbTexture_{};
  rendering::Buffer rayBuffer_{};
  rendering::Buffer screenHitBuffer_{};
  Settings settings_{};
  math::Mat4f viewProjection_ = math::Mat4f::identity();
  rendering::BindingsLayout layout_{};
  rendering::BindingGroups bindingGroups_{};
  rendering::Shader shader_{};
  rendering::ComputePipeline pipeline_{};
  rendering::Buffer uniformBuffer_{};
  std::vector<rendering::BufferId> frameOverrideUniformBufferIds_;
};

} // namespace globalillumination::raytracing
