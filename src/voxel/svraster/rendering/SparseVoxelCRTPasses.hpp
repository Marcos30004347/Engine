#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace voxel::svraster::rendering
{

// CRT tube simulation. See docs/crt-post-process-plan.md.
//
// Five stages in physical order, one pass each:
//
//   A Signal    gamut, encode, bandwidth, subcarrier, ringing, noise
//   B Beam      geometry, scan, spot, defocus, scanlines, persistence
//   C Phosphor  mask coverage, subpixel blending, convergence, beam limiter
//   D Glow      three-scale downsample and blur
//   E Glass     glow composite, tint, reflection, vignette, roll, tonemap
//
// The order is load-bearing rather than cosmetic. Persistence happens at the
// phosphor and must run before the mask; the beam limiter reacts to total
// emitted light and must run after it; glow is an optical process in the glass
// and runs after both. Two stages can each be individually correct and still
// produce a wrong picture if the order is wrong.
//
// Phase 0 ships every stage as a pass-through, so this is one class driven by a
// stage enum rather than five near-identical classes. The stages diverge in
// later phases; the split into separate passes is what makes each one's output
// inspectable through `debugStage` and separately bisectable.
class SparseVoxelCRTPass final : public ::rendering::Pass
{
public:
  enum class Stage : uint32_t
  {
    Signal = 0u,
    Beam = 1u,
    Phosphor = 2u,
    Glow = 3u,
    Glass = 4u,
  };

  // Mirrors CRTUniforms in sparse_voxel_crt_common.wgsl. vec4 blocks only: a
  // vec3<bool> member becomes short3 in MSL and fails pipeline creation rather
  // than compilation, with no useful message.
  struct alignas(16) Uniforms
  {
    std::array<float, 4> extents0{};     // internal w/h, output w/h
    std::array<float, 4> extents1{};     // window w/h, stage, debug stage
    std::array<float, 4> frame{};        // time, frame index, pixel aspect, integer scale
    std::array<float, 4> signalA{};      // path, luma bw, chroma bw, ringing
    std::array<float, 4> signalB{};      // dot crawl, noise, -, -
    std::array<float, 4> geometryA{};    // curvature x/y, pincushion, overscan
    std::array<float, 4> geometryB{};    // linearity x/y, rotation, -
    std::array<float, 4> spotA{};        // spot width min/max, height min/max
    std::array<float, 4> spotB{};        // skew, interlace, persistence, -
    std::array<float, 4> persistence{};  // per-channel decay
    std::array<float, 4> maskA{};        // type, pitch, strength, black matrix
    std::array<float, 4> maskB{};        // convergence, gamut, saturation, -
    std::array<float, 4> limiter{};      // beam limiter, knee, -, -
    std::array<float, 4> glowWeight{};   // level 0/1/2
    std::array<float, 4> glowRadius{};   // level 0/1/2
    std::array<float, 4> halationTint{}; // rgb
    std::array<float, 4> glassA{};       // tint, reflection, corner, vignette
    std::array<float, 4> bezel{};        // rgb
    std::array<float, 4> colorA{};       // gamma, brightness, contrast, saturation
    std::array<float, 4> colorB{};       // black level
    std::array<float, 4> roll{};         // mode, speed, height, strength
  };
  static_assert(sizeof(Uniforms) == 21u * 16u);

  // `inputNames` must match the shader's binding names in order, starting at
  // binding 2. `variant` is the glow pyramid level; it is ignored elsewhere.
  SparseVoxelCRTPass(
      const Stage stage,
      const ::rendering::Texture output,
      const ::rendering::Format outputFormat,
      std::vector<::rendering::Texture> inputs,
      std::vector<std::string> inputNames,
      const uint32_t width,
      const uint32_t height,
      const Uniforms &uniforms,
      const uint32_t variant = 0u)
      : stage_(stage), output_(output), outputFormat_(outputFormat), inputs_(std::move(inputs)), inputNames_(std::move(inputNames)), width_(width), height_(height), values_(uniforms), variant_(variant)
  {
  }

  ~SparseVoxelCRTPass() override
  {
    if (pipeline_.isValid())
      renderGraph->deleteGraphicsPipeline(pipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (groups_.isValid())
      renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    if (sampler_.isValid())
      renderGraph->deleteSampler(sampler_);
    if (uniforms_.isValid())
      renderGraph->deleteBuffer(uniforms_);
  }

  // Live for every parameter except the two resolutions and the master toggle,
  // which change render-target sizes or graph topology and rebuild instead.
  void update(const Uniforms &uniforms)
  {
    values_ = uniforms;
    applyIdentity();
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    applyIdentity();
    renderGraph->bufferWrite(uniforms_, 0u, sizeof(values_), &values_);

    ::rendering::ColorAttachmentInfo color{};
    color.name = passName + "_Color";
    color.view = {
      .texture = output_,
      .access = ::rendering::AccessPattern::COLOR_ATTACHMENT_WRITE,
      .layout = ::rendering::ResourceLayout::COLOR_ATTACHMENT,
      .index = 0u,
      .flags = ::rendering::ImageAspectFlags::Color,
      .baseArrayLayer = 0u,
      .baseMipLevel = 0u,
      .layerCount = 1u,
      .levelCount = 1u,
    };
    color.clearValue = ::rendering::Color::rgba(0.0f, 0.0f, 0.0f, 1.0f);

    ::rendering::RenderPassInfo renderPass{};
    renderPass.name = passName + "_RenderPass";
    renderPass.scissor = ::rendering::Rect2D(0, 0, width_, height_);
    renderPass.viewport = ::rendering::Viewport(width_, height_);
    renderPass.colorAttachments = {color};
    renderPass.depthStencilAttachment.enabled = false;

    commandBuffer.cmdBindGraphicsPipeline(pipeline_);
    commandBuffer.cmdBeginRenderPass(renderPass);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    commandBuffer.cmdDraw(6u, 1u, 0u, 0u);
    commandBuffer.cmdEndRenderPass();
  }

  static const char *stageName(const Stage stage)
  {
    switch (stage)
    {
    case Stage::Signal: return "Signal";
    case Stage::Beam: return "Beam";
    case Stage::Phosphor: return "Phosphor";
    case Stage::Glow: return "Glow";
    case Stage::Glass: return "Glass";
    }
    return "Unknown";
  }

private:
  static const char *spirvPath(const Stage stage)
  {
    switch (stage)
    {
    case Stage::Signal: return "assets/shaders/spirv/svraster/sparse-voxel-crt-signal.spirv";
    case Stage::Beam: return "assets/shaders/spirv/svraster/sparse-voxel-crt-beam.spirv";
    case Stage::Phosphor: return "assets/shaders/spirv/svraster/sparse-voxel-crt-phosphor.spirv";
    case Stage::Glow: return "assets/shaders/spirv/svraster/sparse-voxel-crt-glow.spirv";
    case Stage::Glass: return "assets/shaders/spirv/svraster/sparse-voxel-crt-glass.spirv";
    }
    return "";
  }

  // Which stage this is, and which glow level. `update()` overwrites the block
  // wholesale, so both have to be re-stamped rather than set once.
  void applyIdentity()
  {
    values_.extents1[2] = static_cast<float>(stage_);
    values_.glowRadius[3] = static_cast<float>(variant_);
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;

    ::rendering::BindingGroupLayout group{};
    group.buffers = {
      {.name = "crt",
       .binding = 0u,
       .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
       .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment,
       .isDynamic = false}};
    group.samplers = {
      {.name = "crtSampler", .binding = 1u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment}};
    for (size_t index = 0u; index < inputs_.size(); ++index)
      group.textures.push_back(
          {.name = inputNames_[index],
           .binding = static_cast<uint32_t>(2u + index),
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment});
    layout_ = renderGraph->createBindingsLayout({.name = passName + "_Layout", .groups = {group}});

    // Deliberately NOT frame-local. The binding group is built once and keeps
    // addressing whichever instance existed then, so a frame-local uniform is
    // read from an unwritten instance on the frames where the two disagree -
    // the shader then sees a block of zeros, which reads as a stage ignoring
    // every one of its settings. Rewritten in full every frame below, and
    // consumed in the same frame, so per-frame instances buy nothing.
    uniforms_ = renderGraph->createBuffer(
        {.name = passName + "_Uniforms",
         .size = sizeof(Uniforms),
         .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push,
         .frameLocal = false});

    // Linear throughout. Every stage resamples between extents that differ by a
    // non-integer factor once the internal resolution is a real dial, and a
    // nearest tap there quantizes the beam position to the source grid - which
    // reads as scanline jitter that no beam parameter can remove.
    sampler_ = renderGraph->createSampler(
        {.name = passName + "_Sampler",
         .minFilter = ::rendering::Filter::Linear,
         .magFilter = ::rendering::Filter::Linear,
         .addressModeU = ::rendering::SamplerAddressMode::ClampToEdge,
         .addressModeV = ::rendering::SamplerAddressMode::ClampToEdge,
         .addressModeW = ::rendering::SamplerAddressMode::ClampToEdge,
         .anisotropyEnable = false,
         .maxAnisotropy = 1.0f,
         .maxLod = 1.0f});

    const auto viewOf = [](const ::rendering::Texture texture)
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
        .levelCount = 1u,
      };
    };

    ::rendering::GroupInfo groupInfo{};
    groupInfo.name = "group0";
    groupInfo.buffers = {
      {.bufferView = {.buffer = uniforms_, .offset = 0u, .size = sizeof(Uniforms), .access = ::rendering::AccessPattern::UNIFORM_READ}, .binding = 0u}};
    groupInfo.samplers = {{.sampler = sampler_, .view = viewOf(inputs_.front()), .binding = 1u}};
    for (size_t index = 0u; index < inputs_.size(); ++index)
      groupInfo.textures.push_back({.textureView = viewOf(inputs_[index]), .binding = static_cast<uint32_t>(2u + index)});
    groups_ = renderGraph->createBindingGroups({.layout = layout_, .name = passName + "_Groups", .groups = {groupInfo}});

    shader_ = renderGraph->createShader(
        {.name = passName + "_Shader",
         .layout = layout_,
         .src = os::io::readRelativeFile(spirvPath(stage_)),
         .type = ::rendering::ShaderType::SpirV});

    ::rendering::GraphicsPipelineInfo pipelineInfo{};
    pipelineInfo.name = passName + "_Pipeline";
    pipelineInfo.layout = layout_;
    pipelineInfo.vertexStage.vertexShader = shader_;
    pipelineInfo.vertexStage.shaderEntry = "crtVertex";
    pipelineInfo.vertexStage.cullType = ::rendering::CullMode::None;
    pipelineInfo.vertexStage.winding = ::rendering::WindingOrder::CCW;
    pipelineInfo.vertexStage.primitiveType = ::rendering::PrimitiveType_Triangles;
    pipelineInfo.fragmentStage.fragmentShader = shader_;
    pipelineInfo.fragmentStage.shaderEntry = "crtFragment";
    pipelineInfo.fragmentStage.colorAttatchments = {{
      .format = outputFormat_,
      .loadOp = ::rendering::LoadOp::LoadOp_Clear,
      .storeOp = ::rendering::StoreOp::StoreOp_Store,
      .blendMode = ::rendering::ColorBlendMode::Replace,
      .initialLayout = ::rendering::ResourceLayout::COLOR_ATTACHMENT,
      .finalLayout = ::rendering::ResourceLayout::COLOR_ATTACHMENT,
    }};
    pipelineInfo.fragmentStage.depthAttatchment.enabled = false;
    pipeline_ = renderGraph->createGraphicsPipeline(pipelineInfo);
  }

  Stage stage_ = Stage::Signal;
  ::rendering::Texture output_{};
  ::rendering::Format outputFormat_ = ::rendering::Format::Format_RGBA16Float;
  std::vector<::rendering::Texture> inputs_;
  std::vector<std::string> inputNames_;
  uint32_t width_ = 1u;
  uint32_t height_ = 1u;
  Uniforms values_{};
  uint32_t variant_ = 0u;
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Sampler sampler_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::Shader shader_{};
  ::rendering::GraphicsPipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
