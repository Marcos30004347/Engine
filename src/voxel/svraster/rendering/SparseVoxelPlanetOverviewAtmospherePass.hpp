#pragma once

#include "atmosphere/AtmosphereTypes.hpp"
#include "atmosphere/rendering/AtmosphereGPU.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace voxel::svraster::rendering
{

// Direct atmosphere integration for the normalized whole-planet view. The
// vertex shader expands a fullscreen triangle only over the projected shell
// bounds, so the expensive fragment march scales with the apparent planet.
class SparseVoxelPlanetOverviewAtmospherePass final : public ::rendering::Pass
{
public:
  enum class Diagnostic : uint32_t
  {
    Shaded = 0u,
    Transmittance = 1u,
    Scattering = 2u,
    Bounds = 3u,
  };

  struct Inputs
  {
    ::rendering::Texture overviewColor;
    ::rendering::Texture transmittance;
    ::rendering::Texture multipleScattering;
  };

  struct Settings
  {
    uint32_t width = 1u;
    uint32_t height = 1u;
    bool enabled = false;
    atmosphere::AtmosphereProfile profile{};
    atmosphere::SunSettings sun{};
    float verticalFovRadians = 1.04719755f;
    std::array<float, 4> cameraDirectionDistance{0.0f, 0.0f, 1.0f, 2.2f};
    std::array<float, 3> cameraRight{1.0f, 0.0f, 0.0f};
    std::array<float, 3> cameraUp{0.0f, 1.0f, 0.0f};
    uint32_t integrationSamples = 12u;
    bool starsEnabled = true;
    float starIntensity = 0.35f;
    float sunDiskLuminanceScale = 1.0f;
    Diagnostic diagnostic = Diagnostic::Shaded;
  };

  SparseVoxelPlanetOverviewAtmospherePass(
      const Inputs inputs, const ::rendering::Texture output,
      const Settings settings)
      : inputs_(inputs), output_(output), settings_(sanitize(settings))
  {
  }

  ~SparseVoxelPlanetOverviewAtmospherePass() override
  {
    if (pipeline_.isValid()) renderGraph->deleteGraphicsPipeline(pipeline_);
    if (shader_.isValid()) renderGraph->deleteShader(shader_);
    if (groups_.isValid()) renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid()) renderGraph->deleteBindingsLayout(layout_);
    if (uniforms_.isValid()) renderGraph->deleteBuffer(uniforms_);
  }

  void update(const Settings &settings)
  {
    settings_ = sanitize(settings);
    if (uniforms_.isValid()) upload();
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commands) override
  {
    ensureResources();
    upload();
    ::rendering::ColorAttachmentInfo color{};
    color.name = passName + "_Color";
    color.view = attachment(output_);
    color.clearValue = ::rendering::Color::rgba(0.0f, 0.0f, 0.0f, 0.0f);
    ::rendering::RenderPassInfo pass{};
    pass.name = passName + "_RenderPass";
    pass.scissor = ::rendering::Rect2D(0, 0, settings_.width, settings_.height);
    pass.viewport = ::rendering::Viewport(settings_.width, settings_.height);
    pass.colorAttachments = {color};
    commands.cmdBindGraphicsPipeline(pipeline_);
    commands.cmdBeginRenderPass(pass);
    commands.cmdBindBindingGroups(groups_, nullptr, 0u);
    commands.cmdDraw(3u, 1u, 0u, 0u);
    commands.cmdEndRenderPass();
  }

private:
  struct alignas(16) UniformsGPU
  {
    atmosphere::rendering::AtmosphereParametersGPU atmosphere{};
    std::array<uint32_t, 4> viewport{};
    std::array<uint32_t, 4> bounds{};
    std::array<uint32_t, 4> controls{};
    std::array<float, 4> cameraDirectionDistance{};
    std::array<float, 4> cameraRightFocal{};
    std::array<float, 4> cameraUpStars{};
    std::array<float, 4> display{};
  };
  static_assert(sizeof(UniformsGPU) == 240u);

  static Settings sanitize(Settings settings)
  {
    settings.width = std::max(settings.width, 1u);
    settings.height = std::max(settings.height, 1u);
    settings.verticalFovRadians = std::clamp(
        std::isfinite(settings.verticalFovRadians)
            ? settings.verticalFovRadians : 1.04719755f,
        0.01f, 3.13f);
    settings.cameraDirectionDistance[3] = std::max(
        std::isfinite(settings.cameraDirectionDistance[3])
            ? settings.cameraDirectionDistance[3] : 2.2f,
        1.000001f);
    settings.integrationSamples = std::clamp(
        settings.integrationSamples, 4u, 32u);
    settings.starIntensity = std::max(
        std::isfinite(settings.starIntensity) ? settings.starIntensity : 0.0f,
        0.0f);
    settings.sunDiskLuminanceScale = std::max(
        std::isfinite(settings.sunDiskLuminanceScale)
            ? settings.sunDiskLuminanceScale : 0.0f,
        0.0f);
    return settings;
  }

  static ::rendering::TextureView sampled(const ::rendering::Texture texture)
  {
    return {.texture = texture,
            .access = ::rendering::AccessPattern::SHADER_READ,
            .layout = ::rendering::ResourceLayout::SHADER_READ_ONLY,
            .index = 0u, .flags = ::rendering::ImageAspectFlags::Color,
            .baseArrayLayer = 0u, .baseMipLevel = 0u,
            .layerCount = 1u, .levelCount = 1u};
  }

  static ::rendering::TextureView attachment(const ::rendering::Texture texture)
  {
    auto view = sampled(texture);
    view.access = ::rendering::AccessPattern::COLOR_ATTACHMENT_WRITE;
    view.layout = ::rendering::ResourceLayout::COLOR_ATTACHMENT;
    return view;
  }

  void upload()
  {
    const double ground = std::max(settings_.profile.groundRadiusMetres, 1.0);
    const atmosphere::AtmosphereScreenBounds shell =
        atmosphere::projectedAtmosphereScreenBounds(
            settings_.width, settings_.height, settings_.verticalFovRadians,
            settings_.cameraDirectionDistance[3],
            settings_.profile.topRadiusMetres / ground);
    const float focal = 1.0f /
        std::tan(settings_.verticalFovRadians * 0.5f);
    const UniformsGPU values{
      atmosphere::rendering::makeAtmosphereParametersGPU(
          settings_.profile, settings_.sun),
      {settings_.width, settings_.height,
       settings_.enabled && shell.valid() ? 1u : 0u,
       settings_.integrationSamples},
      {shell.x, shell.y, shell.x + shell.width, shell.y + shell.height},
      {static_cast<uint32_t>(settings_.diagnostic),
       settings_.starsEnabled ? 1u : 0u,
       settings_.sun.drawDisk ? 1u : 0u, 0u},
      settings_.cameraDirectionDistance,
      {settings_.cameraRight[0], settings_.cameraRight[1],
       settings_.cameraRight[2], focal},
      {settings_.cameraUp[0], settings_.cameraUp[1], settings_.cameraUp[2],
       settings_.starIntensity},
      {settings_.sunDiskLuminanceScale, 0.0f, 0.0f, 0.0f},
    };
    renderGraph->bufferWrite(
        uniforms_, 0u, sizeof(values), const_cast<UniformsGPU *>(&values));
  }

  void ensureResources()
  {
    if (layout_.isValid()) return;
    uniforms_ = renderGraph->createBuffer({
      .name = passName + "_Uniforms", .size = sizeof(UniformsGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
          ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true,
    });
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout",
      .groups = {::rendering::BindingGroupLayout{
        .textures = {
          {.name = "overviewColor", .binding = 1u,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment},
          {.name = "transmittanceLut", .binding = 2u,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment},
          {.name = "multipleScatteringLut", .binding = 3u,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment},
        },
        .buffers = {
          {.name = "uniforms", .binding = 0u, .isDynamic = false,
           .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Vertex |
               ::rendering::BindingVisibility::BindingVisibility_Fragment},
        },
      }},
    });
    groups_ = renderGraph->createBindingGroups({
      .name = passName + "_Groups", .layout = layout_,
      .groups = {::rendering::GroupInfo{
        .name = "group0",
        .textures = {
          {.binding = 1u, .textureView = sampled(inputs_.overviewColor)},
          {.binding = 2u, .textureView = sampled(inputs_.transmittance)},
          {.binding = 3u, .textureView = sampled(inputs_.multipleScattering)},
        },
        .buffers = {
          {.binding = 0u, .bufferView = {.buffer = uniforms_, .offset = 0u,
             .size = sizeof(UniformsGPU),
             .access = ::rendering::AccessPattern::UNIFORM_READ}},
        },
      }},
    });
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader", .layout = layout_,
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/sparse-voxel-planet-overview-atmosphere.spirv"),
      .type = ::rendering::ShaderType::SpirV,
    });
    ::rendering::GraphicsPipelineInfo pipeline{};
    pipeline.name = passName + "_Pipeline";
    pipeline.layout = layout_;
    pipeline.vertexStage.vertexShader = shader_;
    pipeline.vertexStage.shaderEntry = "drawOverviewAtmosphereVertex";
    pipeline.vertexStage.cullType = ::rendering::CullMode::None;
    pipeline.vertexStage.winding = ::rendering::WindingOrder::CCW;
    pipeline.vertexStage.primitiveType = ::rendering::PrimitiveType_Triangles;
    pipeline.fragmentStage.fragmentShader = shader_;
    pipeline.fragmentStage.shaderEntry = "drawOverviewAtmosphereFragment";
    pipeline.fragmentStage.colorAttatchments = {{
      .format = ::rendering::Format::Format_RGBA16Float,
      .loadOp = ::rendering::LoadOp::LoadOp_Clear,
      .storeOp = ::rendering::StoreOp::StoreOp_Store,
      .blendMode = ::rendering::ColorBlendMode::Replace,
      .initialLayout = ::rendering::ResourceLayout::COLOR_ATTACHMENT,
      .finalLayout = ::rendering::ResourceLayout::COLOR_ATTACHMENT,
    }};
    pipeline_ = renderGraph->createGraphicsPipeline(pipeline);
  }

  Inputs inputs_{};
  ::rendering::Texture output_{};
  Settings settings_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::GraphicsPipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
