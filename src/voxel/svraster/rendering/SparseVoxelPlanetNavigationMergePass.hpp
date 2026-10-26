#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace voxel::svraster::rendering
{

class SparseVoxelPlanetNavigationMergePass final : public ::rendering::Pass
{
public:
  struct Inputs
  {
    ::rendering::Texture surfaceColor;
    ::rendering::Texture surfaceDepth;
    ::rendering::Texture overviewColor;
    ::rendering::Texture overviewAtmosphere;
    ::rendering::Texture overviewDepth;
  };

  struct Outputs
  {
    ::rendering::Texture color;
    ::rendering::Texture depth;
  };

  struct Settings
  {
    uint32_t width = 1u;
    uint32_t height = 1u;
    float overviewBlend = 0.0f;
    std::array<float, 4> skyHorizonIntensity{0.55f, 0.72f, 1.0f, 1.0f};
    std::array<float, 4> skyZenithExponent{0.08f, 0.24f, 0.55f, 0.35f};
    std::array<float, 4> skyGround{0.05f, 0.045f, 0.04f, 0.0f};
    bool spaceBackground = false;
    bool starsEnabled = true;
    float starIntensity = 0.35f;
    float verticalFovRadians = 1.04719755f;
    std::array<float, 3> cameraDirection{0.0f, 0.0f, 1.0f};
    std::array<float, 3> cameraRight{1.0f, 0.0f, 0.0f};
    std::array<float, 3> cameraUp{0.0f, 1.0f, 0.0f};
    std::array<float, 4> sunDirectionIntensity{0.0f, -1.0f, 0.0f, 1.0f};
    std::array<float, 4> sunColorAngularRadius{1.0f, 1.0f, 1.0f, 0.004675f};
    float sunDiskRadianceScale = 1.0f;
    bool drawSunDisk = true;
  };

  SparseVoxelPlanetNavigationMergePass(const Inputs inputs, const Outputs outputs, const Settings settings) : inputs_(inputs), outputs_(outputs), settings_(settings)
  {
  }

  ~SparseVoxelPlanetNavigationMergePass() override
  {
    if (pipeline_.isValid())
      renderGraph->deleteGraphicsPipeline(pipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (groups_.isValid())
      renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    if (uniformBuffer_.isValid())
      renderGraph->deleteBuffer(uniformBuffer_);
  }

  void update(const Settings &settings)
  {
    settings_ = settings;
    settings_.width = std::max(settings_.width, 1u);
    settings_.height = std::max(settings_.height, 1u);
    settings_.overviewBlend = std::clamp(settings_.overviewBlend, 0.0f, 1.0f);
    if (uniformBuffer_.isValid())
      upload();
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commands) override
  {
    ensureResources();
    upload();
    ::rendering::ColorAttachmentInfo color{};
    color.name = passName + "_Color";
    color.view = colorAttachmentView(outputs_.color);
    color.clearValue = ::rendering::Color::rgba(0.0f, 0.0f, 0.0f, 1.0f);
    ::rendering::RenderPassInfo pass{};
    pass.name = passName + "_RenderPass";
    pass.scissor = ::rendering::Rect2D(0, 0, settings_.width, settings_.height);
    pass.viewport = ::rendering::Viewport(settings_.width, settings_.height);
    pass.colorAttachments = {color};
    pass.depthStencilAttachment = {
      .name = passName + "_Depth",
      .view = depthAttachmentView(outputs_.depth),
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
    std::array<float, 4> viewportBlend{};
    std::array<float, 4> skyHorizonIntensity{};
    std::array<float, 4> skyZenithExponent{};
    std::array<float, 4> skyGround{};
    std::array<float, 4> cameraDirectionFocal{};
    std::array<float, 4> cameraRightStars{};
    std::array<float, 4> cameraUpControls{};
    std::array<float, 4> sunDirectionIntensity{};
    std::array<float, 4> sunColorAngularRadius{};
    std::array<float, 4> sunDisplay{};
  };

  static ::rendering::TextureView sampled(const ::rendering::Texture texture, const bool depth = false)
  {
    return {
      .texture = texture,
      .access = ::rendering::AccessPattern::SHADER_READ,
      .layout = depth ? ::rendering::ResourceLayout::DEPTH_STENCIL_READ_ONLY : ::rendering::ResourceLayout::SHADER_READ_ONLY,
      .index = 0u,
      .flags = depth ? ::rendering::ImageAspectFlags::Depth : ::rendering::ImageAspectFlags::Color,
      .baseArrayLayer = 0u,
      .baseMipLevel = 0u,
      .layerCount = 1u,
      .levelCount = 1u};
  }

  static ::rendering::TextureView colorAttachmentView(const ::rendering::Texture texture)
  {
    auto view = sampled(texture);
    view.access = ::rendering::AccessPattern::COLOR_ATTACHMENT_WRITE;
    view.layout = ::rendering::ResourceLayout::COLOR_ATTACHMENT;
    return view;
  }

  static ::rendering::TextureView depthAttachmentView(const ::rendering::Texture texture)
  {
    auto view = sampled(texture, true);
    view.access = ::rendering::AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE;
    view.layout = ::rendering::ResourceLayout::DEPTH_STENCIL_ATTACHMENT;
    return view;
  }

  void upload()
  {
    UniformsGPU uniforms{};
    uniforms.viewportBlend = {static_cast<float>(settings_.width), static_cast<float>(settings_.height), settings_.overviewBlend, 0.0f};
    uniforms.skyHorizonIntensity = settings_.skyHorizonIntensity;
    uniforms.skyZenithExponent = settings_.skyZenithExponent;
    uniforms.skyGround = settings_.skyGround;
    uniforms.cameraDirectionFocal = {
      settings_.cameraDirection[0], settings_.cameraDirection[1],
      settings_.cameraDirection[2],
      1.0f / std::tan(std::clamp(settings_.verticalFovRadians,
          0.01f, 3.13f) * 0.5f)};
    uniforms.cameraRightStars = {
      settings_.cameraRight[0], settings_.cameraRight[1],
      settings_.cameraRight[2],
      settings_.starsEnabled ? std::max(settings_.starIntensity, 0.0f) : 0.0f};
    uniforms.cameraUpControls = {
      settings_.cameraUp[0], settings_.cameraUp[1], settings_.cameraUp[2],
      settings_.spaceBackground ? 1.0f : 0.0f};
    uniforms.sunDirectionIntensity = settings_.sunDirectionIntensity;
    uniforms.sunColorAngularRadius = settings_.sunColorAngularRadius;
    uniforms.sunDisplay = {
      std::max(settings_.sunDiskRadianceScale, 0.0f),
      settings_.drawSunDisk ? 1.0f : 0.0f, 0.0f, 0.0f};
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(uniforms), &uniforms);
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;
    uniformBuffer_ = renderGraph->createBuffer({.name = passName + "_Uniforms", .size = sizeof(UniformsGPU), .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push});
    layout_ = renderGraph->createBindingsLayout(
        {.name = passName + "_Layout",
         .groups = {::rendering::BindingGroupLayout{
           .textures =
               {
                 {.name = "surfaceColor", .binding = 0u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment},
                 {.name = "surfaceDepth", .binding = 1u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment},
                 {.name = "overviewColor", .binding = 2u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment},
                 {.name = "overviewDepth", .binding = 3u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment},
                 {.name = "overviewAtmosphere", .binding = 5u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment},
               },
           .buffers = {
             {.name = "uniforms", .binding = 4u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment},
           }}}});
    groups_ = renderGraph->createBindingGroups(
        {.layout = layout_,
         .name = passName + "_Groups",
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .textures =
               {
                 {.binding = 0u, .textureView = sampled(inputs_.surfaceColor)},
                 {.binding = 1u, .textureView = sampled(inputs_.surfaceDepth, true)},
                 {.binding = 2u, .textureView = sampled(inputs_.overviewColor)},
                 {.binding = 3u, .textureView = sampled(inputs_.overviewDepth, true)},
                 {.binding = 5u, .textureView = sampled(inputs_.overviewAtmosphere)},
               },
           .buffers = {
             {.binding = 4u, .bufferView = {.buffer = uniformBuffer_, .offset = 0u, .size = sizeof(UniformsGPU), .access = ::rendering::AccessPattern::UNIFORM_READ}},
           }}}});
    shader_ = renderGraph->createShader(
        {.name = passName + "_Shader", .layout = layout_, .src = os::io::readRelativeFile("assets/shaders/spirv/svraster/sparse-voxel-planet-navigation-merge.spirv"), .type = ::rendering::ShaderType::SpirV});
    ::rendering::DepthAttatchment depth{};
    depth.enabled = true;
    depth.comparison = ::rendering::ComparisonOp::ComparisonOp_Always;
    depth.format = ::rendering::Format::Format_Depth32Float;
    depth.loadOp = ::rendering::LoadOp::LoadOp_Clear;
    depth.storeOp = ::rendering::StoreOp::StoreOp_Store;
    ::rendering::GraphicsPipelineInfo pipeline{};
    pipeline.name = passName + "_Pipeline";
    pipeline.layout = layout_;
    pipeline.vertexStage.vertexShader = shader_;
    pipeline.vertexStage.shaderEntry = "drawNavigationMergeVertex";
    pipeline.vertexStage.cullType = ::rendering::CullMode::None;
    pipeline.vertexStage.winding = ::rendering::WindingOrder::CCW;
    pipeline.vertexStage.primitiveType = ::rendering::PrimitiveType_Triangles;
    pipeline.fragmentStage.fragmentShader = shader_;
    pipeline.fragmentStage.shaderEntry = "drawNavigationMergeFragment";
    pipeline.fragmentStage.colorAttatchments = {{
      .format = ::rendering::Format::Format_RGBA16Float,
      .loadOp = ::rendering::LoadOp::LoadOp_Clear,
      .storeOp = ::rendering::StoreOp::StoreOp_Store,
      .blendMode = ::rendering::ColorBlendMode::Replace,
      .initialLayout = ::rendering::ResourceLayout::COLOR_ATTACHMENT,
      .finalLayout = ::rendering::ResourceLayout::COLOR_ATTACHMENT,
    }};
    pipeline.fragmentStage.depthAttatchment = depth;
    pipeline_ = renderGraph->createGraphicsPipeline(pipeline);
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
