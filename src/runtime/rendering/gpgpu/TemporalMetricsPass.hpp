#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

namespace rendering::gpgpu
{

// Reduces the final upscaled image to two comparable numbers. See
// `temporal-metrics-cs.wgsl` for what they mean and what they are not.
//
// This exists because the alternative is tuning an accumulator by eye, and a
// temporal upscaler is exactly the kind of system where that fails: the
// artefacts are motion dependent, they interact, and a change that removes
// shimmer by dropping history looks better in a screenshot and worse in motion.
class TemporalMetricsPass final : public ::rendering::Pass
{
public:
  enum class Stage : uint32_t
  {
    Reset = 0u,
    Measure = 1u,
  };

  struct Settings
  {
    uint32_t width = 1u;
    uint32_t height = 1u;
    // Also reduce the accumulator's history-weight term chain. Requires the
    // upscaler to be writing its debug targets.
    bool terms = false;
  };

  struct Buffers
  {
    // The final upscaled image, and the previous frame's accumulated colour.
    ::rendering::Texture current;
    ::rendering::Texture previous;
    // The accumulator's history-weight terms; placeholders when Settings::terms
    // is off.
    ::rendering::Texture termsA;
    ::rendering::Texture termsB;
    ::rendering::Buffer metrics;
  };

  // Words in the metrics buffer: instability, edge energy, counted pixels, edge
  // pixels, then the eight accumulator term sums, then the per-region grid.
  //
  // The region grid repeats {instability, edge energy, pixels, edge pixels,
  // luminance, lit pixels} for each cell of a 4x4 split of the screen. A
  // whole-frame mean cannot see a defect confined to part of the screen - it
  // dilutes it by the fraction of the frame the defect does not cover - and
  // this renderer has one that covers about a fifth. The last two sums are the
  // ones that see a defect which is *stable*: a persistently wrong region has
  // no frame-to-frame difference and no edge energy to report.
  // Must match the constants in `temporal-metrics-cs.wgsl`.
  static constexpr uint32_t kRegionAxis = 4u;
  static constexpr uint32_t kRegionCount = kRegionAxis * kRegionAxis;
  static constexpr uint32_t kRegionWords = 6u;
  static constexpr double kLuminanceScale = 1024.0;
  static constexpr uint32_t kRegionWordOffset = 12u;
  static constexpr uint32_t kMetricWords =
      kRegionWordOffset + kRegionCount * kRegionWords;
  static constexpr uint32_t kTermWordOffset = 4u;
  static constexpr uint32_t kTermWords = 8u;
  // Matches TEMPORAL_TERM_SCALE in the shader.
  static constexpr double kTermScale = 256.0;

  TemporalMetricsPass(Stage stage, Buffers buffers, Settings settings)
      : stage_(stage), buffers_(buffers), settings_(settings)
  {
  }

  void update(const Settings &settings)
  {
    settings_ = settings;
    if (uniforms_.isValid()) upload();
  }

  ~TemporalMetricsPass() override
  {
    if (pipeline_.isValid()) renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid()) renderGraph->deleteShader(shader_);
    if (groups_.isValid()) renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid()) renderGraph->deleteBindingsLayout(layout_);
    if (uniforms_.isValid()) renderGraph->deleteBuffer(uniforms_);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &cmd) override
  {
    ensure();
    upload();
    cmd.cmdBindComputePipeline(pipeline_);
    cmd.cmdBindBindingGroups(groups_, nullptr, 0u);
    if (stage_ == Stage::Reset)
      cmd.cmdDispatch((kMetricWords + 63u) / 64u, 1u, 1u);
    else
      cmd.cmdDispatch(
          (settings_.width + 7u) / 8u, (settings_.height + 7u) / 8u, 1u);
  }

private:
  struct alignas(16) Uniforms
  {
    std::array<uint32_t, 4> dimensions{};
  };

  void upload()
  {
    if (!uniforms_.isValid()) return;
    const Uniforms values{
      {settings_.width, settings_.height, settings_.terms ? 1u : 0u, 0u}};
    renderGraph->bufferWrite(
        uniforms_, 0u, sizeof(values), const_cast<Uniforms *>(&values));
  }

  void ensure()
  {
    if (layout_.isValid()) return;
    const auto sampled = [](const ::rendering::Texture texture)
    {
      return ::rendering::TextureView{
        .texture = texture,
        .access = ::rendering::AccessPattern::SHADER_READ,
        .layout = ::rendering::ResourceLayout::SHADER_READ_ONLY,
        .index = 0u,
        .flags = ::rendering::ImageAspectFlags::Color,
        .baseArrayLayer = 0u,
        .baseMipLevel = 0u,
        .layerCount = 1u,
        .levelCount = 1u};
    };
    // Not frame-local. `ensure()` builds the binding group once and keeps it,
    // so the group addresses the instance that existed at creation while
    // `upload()` writes whichever instance the frame being recorded resolves
    // to. With two frames in flight the shader read an instance nothing had
    // written on every other frame, saw a zero viewport, and returned from
    // every invocation - so the whole metrics buffer came back zero and every
    // average taken over a frame range was silently halved.
    uniforms_ = renderGraph->createBuffer({
      .name = passName + "_Uniforms",
      .size = sizeof(Uniforms),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
               ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = false});
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout",
      .groups = {::rendering::BindingGroupLayout{
        .buffers = {
          {.name = "settings",
           .binding = 0u,
           .isDynamic = false,
           .type =
               ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
           .visibility =
               ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "metrics",
           .binding = 3u,
           .isDynamic = false,
           .type =
               ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
           .visibility =
               ::rendering::BindingVisibility::BindingVisibility_Compute}},
        .textures = {
          {.name = "currentTexture",
           .binding = 1u,
           .visibility =
               ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "previousTexture",
           .binding = 2u,
           .visibility =
               ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "termsATexture",
           .binding = 4u,
           .visibility =
               ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "termsBTexture",
           .binding = 5u,
           .visibility =
               ::rendering::BindingVisibility::BindingVisibility_Compute}}}}});
    groups_ = renderGraph->createBindingGroups({
      .layout = layout_,
      .name = passName + "_Groups",
      .groups = {::rendering::GroupInfo{
        .name = "group0",
        .buffers = {
          {.binding = 0u,
           .bufferView = {
             .buffer = uniforms_,
             .offset = 0u,
             .size = sizeof(Uniforms),
             .access = ::rendering::AccessPattern::UNIFORM_READ}},
          {.binding = 3u,
           .bufferView = {
             .buffer = buffers_.metrics,
             .offset = 0u,
             .size = kMetricWords * sizeof(uint32_t),
             .access = ::rendering::AccessPattern::SHADER_READ |
                 ::rendering::AccessPattern::SHADER_WRITE}}},
        .textures = {
          {.binding = 1u, .textureView = sampled(buffers_.current)},
          {.binding = 2u, .textureView = sampled(buffers_.previous)},
          {.binding = 4u, .textureView = sampled(buffers_.termsA)},
          {.binding = 5u, .textureView = sampled(buffers_.termsB)}}}}});
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .layout = layout_,
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/temporal-metrics-cs.spirv"),
      .type = ::rendering::ShaderType::SpirV});
    pipeline_ = renderGraph->createComputePipeline({
      .entry = stage_ == Stage::Reset ? "resetTemporalMetrics"
                                      : "computeTemporalMetrics",
      .layout = layout_,
      .name = passName + "_Pipeline",
      .shader = shader_});
  }

  Stage stage_{Stage::Measure};
  Buffers buffers_{};
  Settings settings_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace rendering::gpgpu
