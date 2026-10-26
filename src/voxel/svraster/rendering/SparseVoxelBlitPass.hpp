#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace voxel::svraster::rendering
{

class SparseVoxelBlitPass final : public ::rendering::Pass
{
public:
  // `sourceWidth`/`sourceHeight` are the input's own extent. They only matter
  // when its aspect differs from the output's - the CRT chain can simulate a
  // 4:3 tube into a 16:9 window - in which case the image is fitted and the
  // remainder shows `bezel`. Left at zero, or matching the output aspect, the
  // transform is the identity and this is the copy it has always been.
  SparseVoxelBlitPass(
      const ::rendering::Texture output,
      const ::rendering::Format outputFormat,
      const ::rendering::Texture input,
      const uint32_t width,
      const uint32_t height,
      const ::rendering::Filter filter = ::rendering::Filter::Nearest,
      const uint32_t sourceWidth = 0u,
      const uint32_t sourceHeight = 0u,
      const std::array<float, 3> bezel = {0.0f, 0.0f, 0.0f})
      : output_(output), outputFormat_(outputFormat), input_(input), width_(width), height_(height), filter_(filter), sourceWidth_(sourceWidth), sourceHeight_(sourceHeight), bezel_(bezel)
  {
  }

  ~SparseVoxelBlitPass() override
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

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    upload();
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
    color.clearValue = ::rendering::Color::rgba(0.02f, 0.02f, 0.025f, 1.0f);

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

  // The fit from a source extent into a destination one, as a uv scale and
  // offset. Shared with the caller so the panel and the log describe the same
  // rectangle the shader draws, rather than a second derivation that can drift.
  // Identity - (1, 1, 0, 0) - whenever the aspects match, which is what keeps
  // the ordinary same-extent blit bit-preserving.
  static std::array<float, 4> computeFit(
      const uint32_t sourceWidth, const uint32_t sourceHeight, const uint32_t destWidth, const uint32_t destHeight)
  {
    std::array<float, 4> fit{1.0f, 1.0f, 0.0f, 0.0f};
    if (sourceWidth == 0u || sourceHeight == 0u || destWidth == 0u || destHeight == 0u)
      return fit;
    const double sourceAspect = static_cast<double>(sourceWidth) / static_cast<double>(sourceHeight);
    const double destAspect = static_cast<double>(destWidth) / static_cast<double>(destHeight);
    if (sourceAspect > destAspect)
      fit[1] = static_cast<float>(destAspect / sourceAspect);
    else if (sourceAspect < destAspect)
      fit[0] = static_cast<float>(sourceAspect / destAspect);
    fit[2] = (1.0f - fit[0]) * 0.5f;
    fit[3] = (1.0f - fit[1]) * 0.5f;
    return fit;
  }

private:
  struct alignas(16) Uniforms
  {
    std::array<float, 4> uvScaleOffset{1.0f, 1.0f, 0.0f, 0.0f};
    std::array<float, 4> bezel{0.0f, 0.0f, 0.0f, 1.0f};
  };
  static_assert(sizeof(Uniforms) == 32u);

  void upload()
  {
    if (!uniforms_.isValid())
      return;
    Uniforms values{};
    // Fit rather than fill: the whole source stays visible and the shortfall
    // becomes bezel, never a stretch.
    values.uvScaleOffset = computeFit(sourceWidth_, sourceHeight_, width_, height_);
    values.bezel = {bezel_[0], bezel_[1], bezel_[2], 1.0f};
    renderGraph->bufferWrite(uniforms_, 0u, sizeof(values), &values);
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;
    layout_ = renderGraph->createBindingsLayout(
        {.name = passName + "_Layout",
         .groups = {::rendering::BindingGroupLayout{
           .buffers = {
             {.name = "blit",
              .binding = 2u,
              .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment,
              .isDynamic = false},
           },
           .samplers = {
             {.name = "nearestSampler", .binding = 0u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment},
           },
           .textures = {
             {.name = "inputTexture", .binding = 1u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment},
           }}}});
    // Deliberately NOT frame-local: the binding group below is built once and
    // keeps addressing whichever instance existed then, so a frame-local
    // uniform is read from an unwritten instance on the frames where the two
    // disagree. Here that would zero the uv transform and reject every pixel as
    // out of bounds, painting the whole window bezel.
    uniforms_ = renderGraph->createBuffer(
        {.name = passName + "_Uniforms",
         .size = sizeof(Uniforms),
         .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push,
         .frameLocal = false});
    sampler_ = renderGraph->createSampler(
        {.name = passName + "_Sampler",
         .minFilter = filter_,
         .magFilter = filter_,
         .addressModeU = ::rendering::SamplerAddressMode::ClampToEdge,
         .addressModeV = ::rendering::SamplerAddressMode::ClampToEdge,
         .addressModeW = ::rendering::SamplerAddressMode::ClampToEdge,
         .anisotropyEnable = false,
         .maxAnisotropy = 1.0f,
         .maxLod = 1.0f});
    const ::rendering::TextureView inputView{
      .texture = input_,
      .access = ::rendering::AccessPattern::SHADER_READ,
      .layout = ::rendering::ResourceLayout::SHADER_READ_ONLY,
      .index = 0u,
      .flags = ::rendering::ImageAspectFlags::Color,
      .baseArrayLayer = 0u,
      .baseMipLevel = 0u,
      .layerCount = 1u,
      .levelCount = 1u,
    };
    groups_ = renderGraph->createBindingGroups(
        {.layout = layout_,
         .name = passName + "_Groups",
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = {{.bufferView = {.buffer = uniforms_, .offset = 0u, .size = sizeof(Uniforms), .access = ::rendering::AccessPattern::UNIFORM_READ}, .binding = 2u}},
           .samplers = {{.binding = 0u, .sampler = sampler_, .view = inputView}},
           .textures = {{.textureView = inputView, .binding = 1u}},
         }}});
    shader_ = renderGraph->createShader(
        {.name = passName + "_Shader",
         .layout = layout_,
         .src = os::io::readRelativeFile("assets/shaders/spirv/svraster/sparse-voxel-blit.spirv"),
         .type = ::rendering::ShaderType::SpirV});

    ::rendering::GraphicsPipelineInfo pipelineInfo{};
    pipelineInfo.name = passName + "_Pipeline";
    pipelineInfo.layout = layout_;
    pipelineInfo.vertexStage.vertexShader = shader_;
    pipelineInfo.vertexStage.shaderEntry = "blitVertex";
    pipelineInfo.vertexStage.cullType = ::rendering::CullMode::None;
    pipelineInfo.vertexStage.winding = ::rendering::WindingOrder::CCW;
    pipelineInfo.vertexStage.primitiveType = ::rendering::PrimitiveType_Triangles;
    pipelineInfo.fragmentStage.fragmentShader = shader_;
    pipelineInfo.fragmentStage.shaderEntry = "blitFragment";
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

  ::rendering::Texture output_{};
  ::rendering::Format outputFormat_ = ::rendering::Format::Format_RGBA8Unorm;
  ::rendering::Texture input_{};
  uint32_t width_ = 1u;
  uint32_t height_ = 1u;
  ::rendering::Filter filter_ = ::rendering::Filter::Nearest;
  uint32_t sourceWidth_ = 0u;
  uint32_t sourceHeight_ = 0u;
  std::array<float, 3> bezel_{0.0f, 0.0f, 0.0f};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Sampler sampler_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::Shader shader_{};
  ::rendering::GraphicsPipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
