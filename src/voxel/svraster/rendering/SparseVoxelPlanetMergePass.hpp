#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace voxel::svraster::rendering
{

// Blends the independently shaded voxel and coarse-planet representations at
// the shaded-colour boundary. The planet therefore never participates in
// voxel tracing, lighting, AO, or denoising, while presentation effects still
// receive cross-faded colour and one coherent geometry/depth identity.
class SparseVoxelPlanetMergePass final : public ::rendering::Pass
{
public:
  struct Inputs
  {
    ::rendering::Texture voxelShadedColor;
    ::rendering::Texture voxelGeometryNormal;
    ::rendering::Texture voxelMotion;
    ::rendering::Texture voxelInstanceId;
    ::rendering::Texture voxelTemporalId;
    ::rendering::Texture voxelDepth;
    ::rendering::Texture planetColor;
    ::rendering::Texture planetNormalDepth;
    ::rendering::Texture planetDepth;
    ::rendering::Buffer coverage;
    ::rendering::Buffer linearDepth;
  };

  struct Outputs
  {
    ::rendering::Texture color;
    ::rendering::Texture geometryNormal;
    ::rendering::Texture motion;
    ::rendering::Texture instanceId;
    ::rendering::Texture temporalId;
    ::rendering::Texture depth;
  };

  struct Settings
  {
    uint32_t width = 1u;
    uint32_t height = 1u;
    float transitionStart = 700.0f;
    float transitionEnd = 1000.0f;
    bool enabled = true;
    uint32_t frameIndex = 0u;
    SparseVoxelRasterUniformsGPU raster{};
  };

  SparseVoxelPlanetMergePass(
      const Inputs inputs, const Outputs outputs, const Settings settings)
      : inputs_(inputs), outputs_(outputs), settings_(settings)
  {
  }

  ~SparseVoxelPlanetMergePass() override
  {
    if (pipeline_.isValid()) renderGraph->deleteGraphicsPipeline(pipeline_);
    if (shader_.isValid()) renderGraph->deleteShader(shader_);
    if (groups_.isValid()) renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid()) renderGraph->deleteBindingsLayout(layout_);
    if (uniformBuffer_.isValid()) renderGraph->deleteBuffer(uniformBuffer_);
  }

  void update(const Settings settings)
  {
    settings_ = settings;
    settings_.width = std::max(settings_.width, 1u);
    settings_.height = std::max(settings_.height, 1u);
    settings_.transitionEnd = std::max(
        settings_.transitionEnd, settings_.transitionStart + 1.0e-3f);
    if (uniformBuffer_.isValid()) upload();
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commands) override
  {
    ensureResources();
    upload();
    ::rendering::RenderPassInfo pass{};
    pass.name = passName + "_RenderPass";
    pass.scissor = ::rendering::Rect2D(0, 0, settings_.width, settings_.height);
    pass.viewport = ::rendering::Viewport(settings_.width, settings_.height);
    pass.colorAttachments = {
      colorAttachment("Color", outputs_.color),
      colorAttachment("GeometryNormal", outputs_.geometryNormal),
      colorAttachment("Motion", outputs_.motion),
      colorAttachment("InstanceId", outputs_.instanceId),
      colorAttachment("TemporalId", outputs_.temporalId),
    };
    pass.depthStencilAttachment = {
      .name = passName + "_Depth",
      .view = depthView(outputs_.depth),
      .clearDepth = 0.0f,
      .clearStencil = 0u,
      .enabled = true,
    };
    commands.cmdBindGraphicsPipeline(pipeline_);
    commands.cmdBeginRenderPass(pass);
    commands.cmdBindBindingGroups(groups_, nullptr, 0u);
    commands.cmdDraw(3u, 1u, 0u, 0u);
    commands.cmdEndRenderPass();
  }

private:
  struct alignas(16) UniformsGPU
  {
    std::array<uint32_t, 4> viewport{};
    std::array<float, 4> transition{};
    std::array<float, 16> cameraToWorld{};
    std::array<float, 4> focalPrincipal{};
  };

  static ::rendering::TextureView sampled(const ::rendering::Texture texture)
  {
    return {.texture = texture,
            .access = ::rendering::AccessPattern::SHADER_READ,
            .layout = ::rendering::ResourceLayout::SHADER_READ_ONLY,
            .index = 0u, .flags = ::rendering::ImageAspectFlags::Color,
            .baseArrayLayer = 0u, .baseMipLevel = 0u,
            .layerCount = 1u, .levelCount = 1u};
  }

  static ::rendering::TextureView sampledDepth(const ::rendering::Texture texture)
  {
    auto view = sampled(texture);
    view.flags = ::rendering::ImageAspectFlags::Depth;
    view.layout = ::rendering::ResourceLayout::DEPTH_STENCIL_READ_ONLY;
    return view;
  }

  static ::rendering::ColorAttachmentInfo colorAttachment(
      const char *name, const ::rendering::Texture texture)
  {
    return {.name = name,
            .view = {.texture = texture,
                     .access = ::rendering::AccessPattern::COLOR_ATTACHMENT_WRITE,
                     .layout = ::rendering::ResourceLayout::COLOR_ATTACHMENT,
                     .index = 0u,
                     .flags = ::rendering::ImageAspectFlags::Color,
                     .baseArrayLayer = 0u, .baseMipLevel = 0u,
                     .layerCount = 1u, .levelCount = 1u},
            .clearValue = ::rendering::Color::rgba(0.0f, 0.0f, 0.0f, 0.0f)};
  }

  static ::rendering::TextureView depthView(const ::rendering::Texture texture)
  {
    return {.texture = texture,
            .access = ::rendering::AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE,
            .layout = ::rendering::ResourceLayout::DEPTH_STENCIL_ATTACHMENT,
            .index = 0u, .flags = ::rendering::ImageAspectFlags::Depth,
            .baseArrayLayer = 0u, .baseMipLevel = 0u,
            .layerCount = 1u, .levelCount = 1u};
  }

  void upload()
  {
    UniformsGPU uniforms{};
    uniforms.viewport = {
      settings_.width, settings_.height, settings_.frameIndex, 0u};
    uniforms.transition = {
      settings_.transitionStart, settings_.transitionEnd,
      settings_.enabled ? 1.0f : 0.0f, 0.0f};
    uniforms.cameraToWorld = settings_.raster.cameraToWorld;
    uniforms.focalPrincipal = settings_.raster.focalPrincipal;
    renderGraph->bufferWrite(
        uniformBuffer_, 0u, sizeof(uniforms), &uniforms);
  }

  void ensureResources()
  {
    if (layout_.isValid()) return;
    uniformBuffer_ = renderGraph->createBuffer(
        {.name = passName + "_Uniforms", .size = sizeof(UniformsGPU),
         .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
             ::rendering::BufferUsage::BufferUsage_Push});
    ::rendering::BindingGroupLayout group{};
    group.buffers = {
      {.name = "uniforms", .binding = 0u, .isDynamic = false,
       .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
       .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment},
      {.name = "coverage", .binding = 10u, .isDynamic = false,
       .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
       .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment},
      {.name = "linearDepth", .binding = 11u, .isDynamic = false,
       .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
       .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment},
    };
    for (uint32_t binding = 1u; binding <= 9u; ++binding)
      group.textures.push_back(
          {.name = "surfaceTexture", .binding = binding,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment});
    layout_ = renderGraph->createBindingsLayout(
        {.name = passName + "_Layout", .groups = {group}});
    groups_ = renderGraph->createBindingGroups(
        {.layout = layout_, .name = passName + "_Groups",
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = {
             {.binding = 0u, .bufferView = {.buffer = uniformBuffer_, .offset = 0u,
               .size = sizeof(UniformsGPU),
               .access = ::rendering::AccessPattern::UNIFORM_READ}},
             {.binding = 10u, .bufferView = {.buffer = inputs_.coverage, .offset = 0u,
               .size = uint64_t{settings_.width} * settings_.height * sizeof(uint32_t),
               .access = ::rendering::AccessPattern::SHADER_READ_WRITE}},
             {.binding = 11u, .bufferView = {.buffer = inputs_.linearDepth, .offset = 0u,
               .size = uint64_t{settings_.width} * settings_.height * sizeof(float),
               .access = ::rendering::AccessPattern::SHADER_READ_WRITE}},
           },
           .textures = {
             {.textureView = sampled(inputs_.voxelShadedColor), .binding = 1u},
             {.textureView = sampled(inputs_.voxelGeometryNormal), .binding = 2u},
             {.textureView = sampled(inputs_.voxelMotion), .binding = 3u},
             {.textureView = sampled(inputs_.voxelInstanceId), .binding = 4u},
             {.textureView = sampled(inputs_.voxelTemporalId), .binding = 5u},
             {.textureView = sampledDepth(inputs_.voxelDepth), .binding = 6u},
             {.textureView = sampled(inputs_.planetColor), .binding = 7u},
             {.textureView = sampled(inputs_.planetNormalDepth), .binding = 8u},
             {.textureView = sampledDepth(inputs_.planetDepth), .binding = 9u},
           }}}});
    shader_ = renderGraph->createShader(
        {.name = passName + "_Shader", .layout = layout_,
         .src = os::io::readRelativeFile(
             "assets/shaders/spirv/svraster/sparse-voxel-planet-merge.spirv"),
         .type = ::rendering::ShaderType::SpirV});
    ::rendering::GraphicsPipelineInfo pipeline{};
    pipeline.name = passName + "_Pipeline";
    pipeline.layout = layout_;
    pipeline.vertexStage.vertexShader = shader_;
    pipeline.vertexStage.shaderEntry = "drawMergeVertex";
    pipeline.vertexStage.cullType = ::rendering::CullMode::None;
    pipeline.vertexStage.winding = ::rendering::WindingOrder::CCW;
    pipeline.vertexStage.primitiveType = ::rendering::PrimitiveType_Triangles;
    pipeline.fragmentStage.fragmentShader = shader_;
    pipeline.fragmentStage.shaderEntry = "drawMergeFragment";
    pipeline.fragmentStage.colorAttatchments = {
      colorFormat(::rendering::Format::Format_RGBA16Float),
      colorFormat(::rendering::Format::Format_RGBA16Float),
      colorFormat(::rendering::Format::Format_RG16Float),
      colorFormat(::rendering::Format::Format_R32Uint),
      colorFormat(::rendering::Format::Format_R32Uint),
    };
    pipeline.fragmentStage.depthAttatchment = {
      .enabled = true,
      .format = ::rendering::Format::Format_Depth32Float,
      .loadOp = ::rendering::LoadOp::LoadOp_Clear,
      .storeOp = ::rendering::StoreOp::StoreOp_Store,
      .depthTestEnabled = false,
      .depthWriteEnabled = true,
      .comparison = ::rendering::ComparisonOp::ComparisonOp_Always};
    pipeline_ = renderGraph->createGraphicsPipeline(pipeline);
  }

  static ::rendering::ColorAttatchment colorFormat(
      const ::rendering::Format format)
  {
    return {.format = format,
            .loadOp = ::rendering::LoadOp::LoadOp_Clear,
            .storeOp = ::rendering::StoreOp::StoreOp_Store,
            .blendMode = ::rendering::ColorBlendMode::Replace,
            .initialLayout = ::rendering::ResourceLayout::COLOR_ATTACHMENT,
            .finalLayout = ::rendering::ResourceLayout::COLOR_ATTACHMENT};
  }

  Inputs inputs_{};
  Outputs outputs_{};
  Settings settings_{};
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::GraphicsPipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
