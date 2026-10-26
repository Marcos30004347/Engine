#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace rendering::gpgpu
{

class LightingTemporalAccumulationPass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t width = 1u;
    uint32_t height = 1u;
    float maxHistoryFrames = 32.0f;
    float normalThreshold = 0.8f;
    // Camera motion is allowed a normal bilinear footprint. Coplanar voxel
    // receivers cannot be distinguished by depth/normal at their shared edge,
    // so tightening this gate only rejects useful history and exposes noise.
    float surfaceTolerancePixels = 3.0f;
    // Radius used only when a covered current pixel has neither a current
    // measurement nor valid temporal history. It gathers same-surface current
    // sparse lightfield samples so composition never turns missing phases into
    // black output.
    float currentFallbackRadiusPixels = 4.0f;
    // Accumulation responsiveness while this receiver is moving on screen.
    //
    // Every frame the camera moves, an unmeasured pixel's estimate is
    // reprojected and resampled onto a new position, and only the scheduled
    // fraction of pixels - one in four at half ray rate - takes a measurement
    // that can correct it, each weighted 1/window. A window sized for a
    // stationary camera therefore holds roughly a second of pre-motion,
    // repeatedly resampled radiance, and that is the smear still on screen the
    // moment the camera stops; clearing it then costs another whole window of
    // measurements. Above the motion threshold the running mean uses the
    // shorter window instead, so what survives to the stop is what the
    // receiver actually measured during the motion. Zero on either value
    // restores the single fixed window.
    //
    // Responsiveness only. The published sample count is deliberately
    // unchanged: it is the confidence SVGF reads to size its spatial filter,
    // and collapsing that on camera motion replaces the accumulator's own
    // detail with filter blur.
    float motionResponsePixels = 0.5f;
    float motionHistoryFrames = 2.0f;
    std::array<float, 16u> currentClipToHistoryView{
      1.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 1.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 1.0f, 0.0f,
      0.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 16u> historyClipToHistoryView{
      1.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 1.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 1.0f, 0.0f,
      0.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 16u> currentClipToHistoryClip{
      1.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 1.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 1.0f, 0.0f,
      0.0f, 0.0f, 0.0f, 1.0f};
    bool enabled = true;
    bool historyValid = false;
    bool cameraStationary = false;
  };

  struct Textures
  {
    rendering::Texture currentLighting{};
    rendering::Texture historyLighting{};
    rendering::Texture currentDepth{};
    rendering::Texture currentNormal{};
    rendering::Texture historyDepth{};
    rendering::Texture historyNormal{};
    rendering::Texture currentMotion{};
    rendering::Texture outputLighting{};
  };

  LightingTemporalAccumulationPass(Textures textures, Settings settings)
      : textures_(textures), settings_(settings)
  {
  }

  ~LightingTemporalAccumulationPass() override
  {
    destroy();
  }

  void setSettings(const Settings &settings)
  {
    settings_ = settings;
    if (uniformBuffer_.isValid())
      writeUniforms();
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    createResources();
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0);
    commandBuffer.cmdDispatch(
        (settings_.width + 7u) / 8u,
        (settings_.height + 7u) / 8u,
        1u);
  }

private:
  struct Uniforms
  {
    uint32_t viewport[4] = {1u, 1u, 0u, 0u};
    float params[4] = {32.0f, 0.8f, 0.0f, 0.0f};
    float params2[4] = {4.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 16u> currentClipToHistoryView{};
    std::array<float, 16u> historyClipToHistoryView{};
    std::array<float, 16u> currentClipToHistoryClip{};
  };
  static_assert(sizeof(Uniforms) == 240u);

  static rendering::TextureView sampledColor(rendering::Texture texture)
  {
    return {
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

  static rendering::TextureView sampledDepth(rendering::Texture texture)
  {
    auto view = sampledColor(texture);
    view.flags = rendering::ImageAspectFlags::Depth;
    return view;
  }

  static rendering::TextureView storageColor(rendering::Texture texture)
  {
    auto view = sampledColor(texture);
    view.access = rendering::AccessPattern::SHADER_WRITE;
    view.layout = rendering::ResourceLayout::GENERAL;
    return view;
  }

  void createResources()
  {
    if (layout_.isValid())
      return;
    rendering::BufferInfo uniformInfo{
      .name = passName + "_Uniforms.buffer",
      .size = sizeof(Uniforms),
      .usage = rendering::BufferUsage::BufferUsage_Uniform |
          rendering::BufferUsage::BufferUsage_Push,
    };
    if (renderGraph->getMaxFramesInFlight() > 1u)
      uniformInfo.frameLocal = true;
    uniformBuffer_ = renderGraph->createBuffer(uniformInfo);
    writeUniforms();

    rendering::BindingGroupLayout group{};
    group.buffers = {{
      .name = "uniforms", .binding = 0u, .isDynamic = false,
      .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer,
      .visibility = rendering::BindingVisibility::BindingVisibility_Compute}};
    for (uint32_t binding = 1u; binding <= 7u; ++binding)
      group.textures.push_back({
        .name = "texture" + std::to_string(binding),
        .binding = binding,
        .visibility = rendering::BindingVisibility::BindingVisibility_Compute});
    group.storageTextures = {{
      .name = "outputLighting", .binding = 8u,
      .visibility = rendering::BindingVisibility::BindingVisibility_Compute}};
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout", .groups = {group}});

    const std::array<rendering::Texture, 7u> sampled{
      textures_.currentLighting, textures_.historyLighting,
      textures_.currentDepth, textures_.currentNormal,
      textures_.historyDepth, textures_.historyNormal,
      textures_.currentMotion};
    rendering::GroupInfo groupInfo{.name = "group0"};
    groupInfo.buffers = {{
      .binding = 0u,
      .bufferView = {.buffer = uniformBuffer_, .offset = 0u,
                     .size = sizeof(Uniforms),
                     .access = rendering::AccessPattern::SHADER_READ}}};
    for (uint32_t index = 0u; index < sampled.size(); ++index)
      groupInfo.textures.push_back({
        .binding = index + 1u,
        .textureView = (index == 2u || index == 4u)
            ? sampledDepth(sampled[index])
            : sampledColor(sampled[index])});
    groupInfo.storageTextures = {{
      .binding = 8u, .textureView = storageColor(textures_.outputLighting)}};
    bindingGroups_ = renderGraph->createBindingGroups({
      .layout = layout_, .name = passName + "_BindingGroups",
      .groups = {groupInfo}});
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader", .layout = layout_,
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/lighting-temporal-accumulation-cs.spirv"),
      .type = rendering::ShaderType::SpirV});
    pipeline_ = renderGraph->createComputePipeline({
      .entry = "main", .layout = layout_,
      .name = passName + "_Pipeline", .shader = shader_});
  }

  void writeUniforms()
  {
    const Uniforms uniforms{
      .viewport = {std::max(settings_.width, 1u),
                   std::max(settings_.height, 1u),
                   settings_.enabled ? 1u : 0u,
                   settings_.historyValid ? 1u : 0u},
      .params = {std::max(settings_.maxHistoryFrames, 1.0f),
                 std::clamp(settings_.normalThreshold, -1.0f, 1.0f),
                 std::max(settings_.surfaceTolerancePixels, 0.5f),
                 settings_.cameraStationary ? 1.0f : 0.0f},
      .params2 = {std::clamp(
                      settings_.currentFallbackRadiusPixels, 0.0f, 8.0f),
                  std::max(settings_.motionResponsePixels, 0.0f),
                  std::clamp(
                      settings_.motionHistoryFrames, 0.0f,
                      std::max(settings_.maxHistoryFrames, 1.0f)),
                  0.0f},
      .currentClipToHistoryView = settings_.currentClipToHistoryView,
      .historyClipToHistoryView = settings_.historyClipToHistoryView,
      .currentClipToHistoryClip = settings_.currentClipToHistoryClip};
    renderGraph->bufferWrite(
        uniformBuffer_, 0u, sizeof(uniforms),
        const_cast<Uniforms *>(&uniforms));
  }

  void destroy()
  {
    if (pipeline_.isValid()) renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid()) renderGraph->deleteShader(shader_);
    if (bindingGroups_.isValid()) renderGraph->deleteBindingGroups(bindingGroups_);
    if (layout_.isValid()) renderGraph->deleteBindingsLayout(layout_);
    if (uniformBuffer_.isValid()) renderGraph->deleteBuffer(uniformBuffer_);
  }

  Textures textures_{};
  Settings settings_{};
  rendering::Buffer uniformBuffer_{};
  rendering::BindingsLayout layout_{};
  rendering::BindingGroups bindingGroups_{};
  rendering::Shader shader_{};
  rendering::ComputePipeline pipeline_{};
};

} // namespace rendering::gpgpu
