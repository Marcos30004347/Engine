#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace atmosphere::rendering
{

// Generic presenter for every LUT planned by the atmosphere pipeline. It is
// available before the LUT producers so each later phase gets diagnostics from
// its first frame rather than adding one-off debug shaders.
class AtmosphereLutDebugPass final : public ::rendering::Pass
{
public:
  enum class View : uint32_t
  {
    Disabled = 0u,
    Transmittance = 1u,
    MultipleScattering = 2u,
    SkyView = 3u,
    AerialPerspectiveRadiance = 4u,
    AerialPerspectiveTransmittance = 5u,
  };

  struct Inputs
  {
    ::rendering::Texture transmittance;
    ::rendering::Texture multipleScattering;
    ::rendering::Texture skyView;
    ::rendering::Texture aerialPerspective;
  };

  struct Settings
  {
    uint32_t width = 1u;
    uint32_t height = 1u;
    View view = View::Disabled;
    float aerialSlice = 0.0f;
    float displayScale = 1.0f;
    bool logarithmic = false;
  };

  AtmosphereLutDebugPass(
      Inputs inputs, ::rendering::Texture output, Settings settings)
      : inputs_(inputs), output_(output), settings_(sanitize(settings))
  {
  }

  ~AtmosphereLutDebugPass() override
  {
    if (pipeline_.isValid()) renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid()) renderGraph->deleteShader(shader_);
    if (groups_.isValid()) renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid()) renderGraph->deleteBindingsLayout(layout_);
    if (uniforms_.isValid()) renderGraph->deleteBuffer(uniforms_);
  }

  void update(Settings settings)
  {
    settings_ = sanitize(settings);
    if (uniforms_.isValid()) upload();
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commands) override
  {
    ensureResources();
    upload();
    commands.cmdBindComputePipeline(pipeline_);
    commands.cmdBindBindingGroups(groups_, nullptr, 0u);
    commands.cmdDispatch((settings_.width + 7u) / 8u,
        (settings_.height + 7u) / 8u, 1u);
  }

private:
  struct alignas(16) UniformsGPU
  {
    std::array<uint32_t, 4> viewportMode{};
    std::array<float, 4> display{};
  };

  static Settings sanitize(Settings settings)
  {
    settings.width = std::max(settings.width, 1u);
    settings.height = std::max(settings.height, 1u);
    settings.aerialSlice = std::clamp(settings.aerialSlice, 0.0f, 1.0f);
    settings.displayScale = std::max(settings.displayScale, 0.0f);
    return settings;
  }

  static ::rendering::TextureView sampled(const ::rendering::Texture texture)
  {
    return {.texture = texture,
            .index = 0u,
            .flags = ::rendering::ImageAspectFlags::Color,
            .baseMipLevel = 0u,
            .levelCount = 1u,
            .baseArrayLayer = 0u,
            .layerCount = 1u,
            .access = ::rendering::AccessPattern::SHADER_READ,
            .layout = ::rendering::ResourceLayout::SHADER_READ_ONLY};
  }

  static ::rendering::TextureView storage(const ::rendering::Texture texture)
  {
    return {.texture = texture,
            .index = 0u,
            .flags = ::rendering::ImageAspectFlags::Color,
            .baseMipLevel = 0u,
            .levelCount = 1u,
            .baseArrayLayer = 0u,
            .layerCount = 1u,
            .access = ::rendering::AccessPattern::SHADER_WRITE,
            .layout = ::rendering::ResourceLayout::GENERAL};
  }

  void upload()
  {
    const UniformsGPU values{
      {settings_.width, settings_.height,
       static_cast<uint32_t>(settings_.view),
       settings_.logarithmic ? 1u : 0u},
      {settings_.aerialSlice, settings_.displayScale, 0.0f, 0.0f},
    };
    renderGraph->bufferWrite(uniforms_, 0u, sizeof(values),
        const_cast<UniformsGPU *>(&values));
  }

  void ensureResources()
  {
    if (layout_.isValid()) return;
    uniforms_ = renderGraph->createBuffer({
      .name = passName + "_Uniforms",
      .size = sizeof(UniformsGPU),
      .usage = static_cast<::rendering::BufferUsage>(
          ::rendering::BufferUsage::BufferUsage_Uniform |
          ::rendering::BufferUsage::BufferUsage_Push),
      .frameLocal = true,
    });
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout",
      .groups = {::rendering::BindingGroupLayout{
        .buffers = {
          {.name = "uniforms", .binding = 0u,
           .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
           .isDynamic = false},
        },
        .textures = {
          {.name = "transmittance", .binding = 1u,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "multipleScattering", .binding = 2u,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "skyView", .binding = 3u,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "aerialPerspective", .binding = 4u,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        },
        .storageTextures = {
          {.name = "output", .binding = 5u,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        },
      }},
    });
    groups_ = renderGraph->createBindingGroups({
      .name = passName + "_Groups",
      .layout = layout_,
      .groups = {::rendering::GroupInfo{
        .name = "group0",
        .buffers = {
          {.bufferView = {.buffer = uniforms_, .offset = 0u,
            .size = sizeof(UniformsGPU),
            .access = ::rendering::AccessPattern::UNIFORM_READ},
           .binding = 0u},
        },
        .textures = {
          {.textureView = sampled(inputs_.transmittance), .binding = 1u},
          {.textureView = sampled(inputs_.multipleScattering), .binding = 2u},
          {.textureView = sampled(inputs_.skyView), .binding = 3u},
          {.textureView = sampled(inputs_.aerialPerspective), .binding = 4u},
        },
        .storageTextures = {
          {.textureView = storage(output_), .binding = 5u},
        },
      }},
    });
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/atmosphere/atmosphere-lut-debug.spirv"),
      .type = ::rendering::ShaderType::SpirV,
      .layout = layout_,
    });
    pipeline_ = renderGraph->createComputePipeline({
      .name = passName + "_Pipeline",
      .shader = shader_,
      .entry = "presentAtmosphereLut",
      .layout = layout_,
    });
  }

  Inputs inputs_{};
  ::rendering::Texture output_{};
  Settings settings_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace atmosphere::rendering
