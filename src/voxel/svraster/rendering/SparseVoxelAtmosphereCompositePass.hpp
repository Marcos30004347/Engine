#pragma once

#include "atmosphere/AtmosphereTypes.hpp"
#include "atmosphere/rendering/AtmosphereGPU.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace voxel::svraster::rendering
{

// Unified atmosphere presentation. It belongs after PlanetMerge so both voxel
// and coarse-planet surfaces receive the same aerial-perspective equation,
// while uncovered pixels use the Sky-View LUT and optional solar disk.
class SparseVoxelAtmosphereCompositePass final : public ::rendering::Pass
{
public:
  struct Inputs
  {
    ::rendering::Texture surfaceColor;
    ::rendering::Texture transmittance;
    ::rendering::Texture skyView;
    ::rendering::Texture aerialPerspective;
    ::rendering::Texture reactiveMask;
    ::rendering::Buffer coverage;
    ::rendering::Buffer linearDepth;
  };

  struct Settings
  {
    uint32_t width = 1u;
    uint32_t height = 1u;
    bool enabled = true;
    atmosphere::AtmosphereProfile profile{};
    atmosphere::SunSettings sun{};
    float cameraAltitudeMetres = 2.0f;
    float aerialMaximumDistanceMetres = 32'000.0f;
    float aerialDistributionScaleMetres = 100.0f;
    bool starsEnabled = true;
    float starIntensity = 0.35f;
    float reactiveStrength = 0.0f;
    bool fullReactive = false;
    std::array<float, 3> localUpWorld{0.0f, 1.0f, 0.0f};
    SparseVoxelRasterUniformsGPU raster{};
  };

  SparseVoxelAtmosphereCompositePass(Inputs inputs,
      ::rendering::Texture output, Settings settings)
      : inputs_(inputs), output_(output), settings_(sanitize(settings))
  {
  }

  ~SparseVoxelAtmosphereCompositePass() override
  {
    if (pipeline_.isValid()) renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid()) renderGraph->deleteShader(shader_);
    if (groups_.isValid()) renderGraph->deleteBindingGroups(groups_);
    if (aerialSampler_.isValid()) renderGraph->deleteSampler(aerialSampler_);
    if (skyViewSampler_.isValid()) renderGraph->deleteSampler(skyViewSampler_);
    if (layout_.isValid()) renderGraph->deleteBindingsLayout(layout_);
    if (uniforms_.isValid()) renderGraph->deleteBuffer(uniforms_);
    if (raster_.isValid()) renderGraph->deleteBuffer(raster_);
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
    commands.cmdDispatch((settings_.width + 15u) / 16u,
        (settings_.height + 15u) / 16u, 1u);
  }

private:
  struct alignas(16) UniformsGPU
  {
    atmosphere::rendering::AtmosphereParametersGPU atmosphere{};
    std::array<uint32_t, 4> viewportFlags{};
    std::array<float, 4> localUpCameraRadius{};
    std::array<float, 4> display{};
    std::array<float, 4> aerial{};
  };
  static_assert(sizeof(UniformsGPU) == 192u);

  static Settings sanitize(Settings settings)
  {
    settings.width = std::max(settings.width, 1u);
    settings.height = std::max(settings.height, 1u);
    const float lengthSquared = settings.localUpWorld[0] * settings.localUpWorld[0] +
        settings.localUpWorld[1] * settings.localUpWorld[1] +
        settings.localUpWorld[2] * settings.localUpWorld[2];
    if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-12f)
      settings.localUpWorld = {0.0f, 1.0f, 0.0f};
    else
    {
      const float inverseLength = 1.0f / std::sqrt(lengthSquared);
      for (float &component : settings.localUpWorld)
        component *= inverseLength;
    }
    const float maximumAltitude = static_cast<float>(
        settings.profile.topRadiusMetres - settings.profile.groundRadiusMetres);
    settings.cameraAltitudeMetres = std::clamp(
        std::isfinite(settings.cameraAltitudeMetres)
            ? settings.cameraAltitudeMetres : 0.0f,
        0.0f, std::max(maximumAltitude, 0.0f));
    settings.aerialMaximumDistanceMetres = std::max(
        std::isfinite(settings.aerialMaximumDistanceMetres)
            ? settings.aerialMaximumDistanceMetres : 0.0f,
        1.0f);
    settings.aerialDistributionScaleMetres = std::max(
        std::isfinite(settings.aerialDistributionScaleMetres)
            ? settings.aerialDistributionScaleMetres : 0.0f,
        0.001f);
    return settings;
  }

  static ::rendering::TextureView sampled(const ::rendering::Texture texture)
  {
    return {.texture = texture, .index = 0u,
            .flags = ::rendering::ImageAspectFlags::Color,
            .baseMipLevel = 0u, .levelCount = 1u,
            .baseArrayLayer = 0u, .layerCount = 1u,
            .access = ::rendering::AccessPattern::SHADER_READ,
            .layout = ::rendering::ResourceLayout::SHADER_READ_ONLY};
  }

  static ::rendering::TextureView storage(const ::rendering::Texture texture)
  {
    return {.texture = texture, .index = 0u,
            .flags = ::rendering::ImageAspectFlags::Color,
            .baseMipLevel = 0u, .levelCount = 1u,
            .baseArrayLayer = 0u, .layerCount = 1u,
            .access = ::rendering::AccessPattern::SHADER_WRITE,
            .layout = ::rendering::ResourceLayout::GENERAL};
  }

  void upload()
  {
    const float cameraRadiusKm =
        static_cast<float>(settings_.profile.groundRadiusMetres) * 0.001f +
        settings_.cameraAltitudeMetres * 0.001f;
    const UniformsGPU values{
      atmosphere::rendering::makeAtmosphereParametersGPU(
          settings_.profile, settings_.sun),
      {settings_.width, settings_.height, settings_.enabled ? 1u : 0u,
       settings_.sun.drawDisk ? 1u : 0u},
      {settings_.localUpWorld[0], settings_.localUpWorld[1],
       settings_.localUpWorld[2], cameraRadiusKm},
      {std::max(settings_.sun.diskLuminanceScale, 0.0f),
       settings_.starsEnabled ? std::max(settings_.starIntensity, 0.0f) : 0.0f,
       std::clamp(settings_.reactiveStrength, 0.0f, 1.0f),
       settings_.fullReactive ? 1.0f : 0.0f},
      {settings_.aerialMaximumDistanceMetres * 0.001f,
       settings_.aerialDistributionScaleMetres * 0.001f, 0.0f, 0.0f},
    };
    renderGraph->bufferWrite(uniforms_, 0u, sizeof(values),
        const_cast<UniformsGPU *>(&values));
    renderGraph->bufferWrite(
        raster_, 0u, sizeof(settings_.raster), &settings_.raster);
  }

  void ensureResources()
  {
    if (layout_.isValid()) return;
    const auto uniformUsage = static_cast<::rendering::BufferUsage>(
        ::rendering::BufferUsage::BufferUsage_Uniform |
        ::rendering::BufferUsage::BufferUsage_Push);
    uniforms_ = renderGraph->createBuffer({
      .name = passName + "_Uniforms", .size = sizeof(UniformsGPU),
      .usage = uniformUsage, .frameLocal = true,
    });
    raster_ = renderGraph->createBuffer({
      .name = passName + "_Raster", .size = sizeof(settings_.raster),
      .usage = uniformUsage, .frameLocal = true,
    });
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout",
      .groups = {::rendering::BindingGroupLayout{
        .buffers = {
          {.name = "uniforms", .binding = 0u,
           .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
           .isDynamic = false},
          {.name = "raster", .binding = 1u,
           .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
           .isDynamic = false},
          {.name = "coverage", .binding = 7u,
           .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
           .isDynamic = false},
          {.name = "linearDepth", .binding = 8u,
           .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
           .isDynamic = false},
        },
        .samplers = {
          {.name = "aerialSampler", .binding = 6u,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "skyViewSampler", .binding = 11u,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        },
        .textures = {
          {.name = "surfaceColor", .binding = 2u,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "transmittance", .binding = 3u,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "skyView", .binding = 4u,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "aerialPerspective", .binding = 5u,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        },
        .storageTextures = {
          {.name = "output", .binding = 9u,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "reactiveMask", .binding = 10u,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        },
      }},
    });
    aerialSampler_ = renderGraph->createSampler({
      .name = passName + "_AerialSampler",
      .minFilter = ::rendering::Filter::Linear,
      .magFilter = ::rendering::Filter::Linear,
      .addressModeU = ::rendering::SamplerAddressMode::ClampToEdge,
      .addressModeV = ::rendering::SamplerAddressMode::ClampToEdge,
      .addressModeW = ::rendering::SamplerAddressMode::ClampToEdge,
      .anisotropyEnable = false,
      .maxAnisotropy = 1.0f,
      .maxLod = 0.0f,
    });
    skyViewSampler_ = renderGraph->createSampler({
      .name = passName + "_SkyViewSampler",
      .minFilter = ::rendering::Filter::Linear,
      .magFilter = ::rendering::Filter::Linear,
      .addressModeU = ::rendering::SamplerAddressMode::Repeat,
      .addressModeV = ::rendering::SamplerAddressMode::ClampToEdge,
      .addressModeW = ::rendering::SamplerAddressMode::ClampToEdge,
      .anisotropyEnable = false,
      .maxAnisotropy = 1.0f,
      .maxLod = 0.0f,
    });
    const uint64_t pixelCount = uint64_t{settings_.width} * settings_.height;
    groups_ = renderGraph->createBindingGroups({
      .name = passName + "_Groups", .layout = layout_,
      .groups = {::rendering::GroupInfo{
        .name = "group0",
        .buffers = {
          {.bufferView = {.buffer = uniforms_, .offset = 0u,
             .size = sizeof(UniformsGPU),
             .access = ::rendering::AccessPattern::UNIFORM_READ}, .binding = 0u},
          {.bufferView = {.buffer = raster_, .offset = 0u,
             .size = sizeof(settings_.raster),
             .access = ::rendering::AccessPattern::UNIFORM_READ}, .binding = 1u},
          {.bufferView = {.buffer = inputs_.coverage, .offset = 0u,
             .size = pixelCount * sizeof(uint32_t),
             .access = ::rendering::AccessPattern::SHADER_READ}, .binding = 7u},
          {.bufferView = {.buffer = inputs_.linearDepth, .offset = 0u,
             .size = pixelCount * sizeof(float),
             .access = ::rendering::AccessPattern::SHADER_READ}, .binding = 8u},
        },
        .samplers = {
          {.binding = 6u, .sampler = aerialSampler_,
           .view = sampled(inputs_.aerialPerspective)},
          {.binding = 11u, .sampler = skyViewSampler_,
           .view = sampled(inputs_.skyView)},
        },
        .textures = {
          {.textureView = sampled(inputs_.surfaceColor), .binding = 2u},
          {.textureView = sampled(inputs_.transmittance), .binding = 3u},
          {.textureView = sampled(inputs_.skyView), .binding = 4u},
          {.textureView = sampled(inputs_.aerialPerspective), .binding = 5u},
        },
        .storageTextures = {
          {.textureView = storage(output_), .binding = 9u},
          {.textureView = {.texture = inputs_.reactiveMask, .index = 0u,
             .flags = ::rendering::ImageAspectFlags::Color,
             .baseMipLevel = 0u, .levelCount = 1u,
             .baseArrayLayer = 0u, .layerCount = 1u,
             .access = ::rendering::AccessPattern::SHADER_READ_WRITE,
             .layout = ::rendering::ResourceLayout::GENERAL},
           .binding = 10u},
        },
      }},
    });
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/sparse-voxel-atmosphere-composite.spirv"),
      .type = ::rendering::ShaderType::SpirV,
      .layout = layout_,
    });
    pipeline_ = renderGraph->createComputePipeline({
      .name = passName + "_Pipeline", .shader = shader_,
      .entry = "compositeSparseVoxelAtmosphere", .layout = layout_,
    });
  }

  Inputs inputs_{};
  ::rendering::Texture output_{};
  Settings settings_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::Buffer raster_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Sampler aerialSampler_{};
  ::rendering::Sampler skyViewSampler_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
