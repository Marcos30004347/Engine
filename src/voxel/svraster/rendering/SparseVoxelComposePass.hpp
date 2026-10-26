#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

namespace voxel::svraster::rendering
{

// Combines the denoised, material-independent light multiplier with the
// G-buffer to produce the shaded image, and composites the environment where
// no receiver was hit.
//
// This pass is the boundary between the lighting domain and the image domain.
// Upstream, temporal accumulation is validated by geometry identity (SVGF);
// downstream, a temporal upscaler may run on shaded colour, which is what its
// neighbourhood and luma heuristics are designed for. Running the upscaler on
// the light buffer instead double-accumulates the lighting under weaker
// validation and ghosts across disocclusions.
class SparseVoxelComposePass final : public ::rendering::Pass
{
public:
  struct SkySettings
  {
    std::array<float, 3> up{0.0f, 1.0f, 0.0f};
    std::array<float, 3> horizon{0.55f, 0.72f, 1.0f};
    std::array<float, 3> zenith{0.08f, 0.24f, 0.55f};
    std::array<float, 3> ground{0.05f, 0.045f, 0.04f};
    float exponent = 0.35f;
    float intensity = 1.0f;
    bool enabled = true;
  };

  struct Settings
  {
    uint32_t width = 1u;
    uint32_t height = 1u;
    bool applyAlbedo = true;
    bool applyEmissive = true;
    bool applySkyBackground = true;
    bool applyDistanceFog = false;
    // World-space ray distance. The fog colour is the analytic sky radiance
    // for the pixel's stable, unjittered view direction.
    float fogStartDistance = 500.0f;
    float fogEndDistance = 1000.0f;
    float fogExponent = 1.0f;
    float fogMaximumOpacity = 1.0f;
    bool fogUseSkyColor = true;
    std::array<float, 3> fogColor{0.55f, 0.72f, 1.0f};
    SkySettings sky{};
    SparseVoxelRasterUniformsGPU raster{};
  };

  SparseVoxelComposePass(
      const ::rendering::Texture lighting,
      const ::rendering::Texture baseColor,
      const ::rendering::Texture emissive,
      const ::rendering::Buffer coverage,
      const ::rendering::Buffer linearDepth,
      const ::rendering::Texture output,
      Settings settings)
      : lighting_(lighting), baseColor_(baseColor), emissive_(emissive),
        coverage_(coverage), linearDepth_(linearDepth), output_(output),
        settings_(sanitize(settings))
  {
  }

  ~SparseVoxelComposePass() override
  {
    if (pipeline_.isValid())
      renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (groups_.isValid())
      renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    if (uniformBuffer_.isValid())
      renderGraph->deleteBuffer(uniformBuffer_);
    if (rasterBuffer_.isValid())
      renderGraph->deleteBuffer(rasterBuffer_);
  }

  void update(
      const SparseVoxelRasterUniformsGPU &raster,
      const SkySettings &sky,
      const bool applyDistanceFog,
      const float fogStartDistance,
      const float fogEndDistance,
      const float fogExponent,
      const float fogMaximumOpacity,
      const bool fogUseSkyColor,
      const std::array<float, 3> &fogColor)
  {
    settings_.raster = raster;
    settings_.sky = sanitizeSky(sky);
    settings_.applyDistanceFog = applyDistanceFog;
    settings_.fogStartDistance = fogStartDistance;
    settings_.fogEndDistance = fogEndDistance;
    settings_.fogExponent = fogExponent;
    settings_.fogMaximumOpacity = fogMaximumOpacity;
    settings_.fogUseSkyColor = fogUseSkyColor;
    settings_.fogColor = fogColor;
    settings_ = sanitize(settings_);
    if (rasterBuffer_.isValid())
      upload();
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    upload();
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    commandBuffer.cmdDispatch((settings_.width + 15u) / 16u, (settings_.height + 15u) / 16u, 1u);
  }

private:
  struct alignas(16) UniformsGPU
  {
    uint32_t width;
    uint32_t height;
    uint32_t flags;
    uint32_t padding;
    std::array<float, 4> skyUpExponent;
    std::array<float, 4> skyHorizonIntensity;
    std::array<float, 4> skyZenith;
    std::array<float, 4> skyGround;
    std::array<float, 4> fogDistances;
    std::array<float, 4> fogColorOpacity;
  };
  static_assert(sizeof(UniformsGPU) == 112u);

  static constexpr uint32_t kFlagApplyAlbedo = 1u << 0u;
  static constexpr uint32_t kFlagApplyEmissive = 1u << 1u;
  static constexpr uint32_t kFlagApplySkyBackground = 1u << 2u;
  static constexpr uint32_t kFlagApplyDistanceFog = 1u << 3u;
  static constexpr uint32_t kFlagFogUsesSkyColor = 1u << 4u;

  static Settings sanitize(Settings settings)
  {
    settings.width = std::max(1u, settings.width);
    settings.height = std::max(1u, settings.height);
    settings.sky = sanitizeSky(settings.sky);
    settings.fogStartDistance =
        std::isfinite(settings.fogStartDistance)
            ? std::max(settings.fogStartDistance, 0.0f)
            : 0.0f;
    settings.fogEndDistance =
        std::isfinite(settings.fogEndDistance)
            ? std::max(
                  settings.fogEndDistance,
                  settings.fogStartDistance + 1.0e-3f)
            : settings.fogStartDistance + 1.0f;
    settings.fogExponent =
        std::isfinite(settings.fogExponent)
            ? std::clamp(settings.fogExponent, 0.1f, 8.0f)
            : 1.0f;
    settings.fogMaximumOpacity =
        std::isfinite(settings.fogMaximumOpacity)
            ? std::clamp(settings.fogMaximumOpacity, 0.0f, 1.0f)
            : 1.0f;
    for (float &component : settings.fogColor)
      component = std::isfinite(component)
          ? std::max(component, 0.0f)
          : 0.0f;
    return settings;
  }

  static SkySettings sanitizeSky(SkySettings sky)
  {
    const float lengthSquared =
        sky.up[0] * sky.up[0] + sky.up[1] * sky.up[1] +
        sky.up[2] * sky.up[2];
    if (std::isfinite(lengthSquared) && lengthSquared > 1.0e-12f)
    {
      const float inverseLength = 1.0f / std::sqrt(lengthSquared);
      for (float &component : sky.up)
        component *= inverseLength;
    }
    else
    {
      sky.up = {0.0f, 1.0f, 0.0f};
    }
    const auto sanitizeColor = [](std::array<float, 3> &color)
    {
      for (float &component : color)
        component = std::isfinite(component) ? std::max(component, 0.0f) : 0.0f;
    };
    sanitizeColor(sky.horizon);
    sanitizeColor(sky.zenith);
    sanitizeColor(sky.ground);
    sky.exponent = std::isfinite(sky.exponent) ? std::max(sky.exponent, 0.01f) : 0.35f;
    sky.intensity = std::isfinite(sky.intensity) ? std::max(sky.intensity, 0.0f) : 0.0f;
    return sky;
  }

  static ::rendering::TextureView sampledView(const ::rendering::Texture texture)
  {
    return {
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
  }

  static ::rendering::TextureView storageView(const ::rendering::Texture texture)
  {
    return {
      .texture = texture,
      .access = ::rendering::AccessPattern::SHADER_WRITE,
      .layout = ::rendering::ResourceLayout::GENERAL,
      .index = 0u,
      .flags = ::rendering::ImageAspectFlags::Color,
      .baseArrayLayer = 0u,
      .baseMipLevel = 0u,
      .layerCount = 1u,
      .levelCount = 1u,
    };
  }

  void upload()
  {
    if (!uniformBuffer_.isValid())
      return;
    UniformsGPU uniforms{};
    uniforms.width = settings_.width;
    uniforms.height = settings_.height;
    uniforms.flags =
        (settings_.applyAlbedo ? kFlagApplyAlbedo : 0u) |
        (settings_.applyEmissive ? kFlagApplyEmissive : 0u) |
        (settings_.applySkyBackground && settings_.sky.enabled
             ? kFlagApplySkyBackground
             : 0u) |
        (settings_.applyDistanceFog &&
                 (!settings_.fogUseSkyColor || settings_.sky.enabled)
             ? kFlagApplyDistanceFog
             : 0u) |
        (settings_.fogUseSkyColor && settings_.sky.enabled
             ? kFlagFogUsesSkyColor
             : 0u);
    const auto &cameraToWorld = settings_.raster.cameraToWorld;
    uniforms.skyUpExponent = {
        cameraToWorld[0] * settings_.sky.up[0] +
            cameraToWorld[1] * settings_.sky.up[1] +
            cameraToWorld[2] * settings_.sky.up[2],
        cameraToWorld[4] * settings_.sky.up[0] +
            cameraToWorld[5] * settings_.sky.up[1] +
            cameraToWorld[6] * settings_.sky.up[2],
        cameraToWorld[8] * settings_.sky.up[0] +
            cameraToWorld[9] * settings_.sky.up[1] +
            cameraToWorld[10] * settings_.sky.up[2],
        settings_.sky.exponent};
    uniforms.skyHorizonIntensity = {
        settings_.sky.horizon[0], settings_.sky.horizon[1],
        settings_.sky.horizon[2], settings_.sky.intensity};
    uniforms.skyZenith = {
        settings_.sky.zenith[0], settings_.sky.zenith[1],
        settings_.sky.zenith[2], 0.0f};
    uniforms.skyGround = {
        settings_.sky.ground[0], settings_.sky.ground[1],
        settings_.sky.ground[2], 0.0f};
    uniforms.fogDistances = {
        settings_.fogStartDistance, settings_.fogEndDistance,
        settings_.fogExponent, 0.0f};
    uniforms.fogColorOpacity = {
        settings_.fogColor[0], settings_.fogColor[1],
        settings_.fogColor[2], settings_.fogMaximumOpacity};
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(uniforms), const_cast<UniformsGPU *>(&uniforms));
    renderGraph->bufferWrite(rasterBuffer_, 0u, sizeof(settings_.raster), &settings_.raster);
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;
    // Deliberately NOT frameLocal. This buffer is written from
    // recordCommandBuffer(); making it frame-local here leaves slots that were
    // not current at that moment reading zeroes (width == 0, so the shader
    // returns without writing any pixel). Raster data follows the existing
    // frame-local upload path below.
    uniformBuffer_ = renderGraph->createBuffer(
        {.name = passName + "_Uniforms.buffer", .size = sizeof(UniformsGPU), .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push});
    rasterBuffer_ = renderGraph->createBuffer(
        {.name = passName + "_Raster.buffer", .size = sizeof(settings_.raster), .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push, .frameLocal = true});
    layout_ = renderGraph->createBindingsLayout(
        {.name = passName + "_Layout",
         .groups = {::rendering::BindingGroupLayout{
           .buffers = {
             {.name = "uniforms", .binding = 0u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "raster", .binding = 1u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "coverage", .binding = 5u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "linearDepth", .binding = 6u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
           },
           .textures = {
             {.name = "lightingTexture", .binding = 2u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "baseColorTexture", .binding = 3u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "emissiveTexture", .binding = 4u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
           },
           .storageTextures = {
             {.name = "outputTexture", .binding = 7u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
           },
         }}});
    const uint64_t pixelCount = std::max<uint64_t>(1u, uint64_t{settings_.width} * settings_.height);
    groups_ = renderGraph->createBindingGroups(
        {.layout = layout_,
         .name = passName + "_Groups",
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = {
             {.binding = 0u, .bufferView = {.buffer = uniformBuffer_, .offset = 0u, .size = sizeof(UniformsGPU), .access = ::rendering::AccessPattern::UNIFORM_READ}},
             {.binding = 1u, .bufferView = {.buffer = rasterBuffer_, .offset = 0u, .size = sizeof(settings_.raster), .access = ::rendering::AccessPattern::UNIFORM_READ}},
             {.binding = 5u, .bufferView = {.buffer = coverage_, .offset = 0u, .size = pixelCount * sizeof(uint32_t), .access = ::rendering::AccessPattern::SHADER_READ}},
             {.binding = 6u, .bufferView = {.buffer = linearDepth_, .offset = 0u, .size = pixelCount * sizeof(float), .access = ::rendering::AccessPattern::SHADER_READ}},
           },
           .textures = {
             {.textureView = sampledView(lighting_), .binding = 2u},
             {.textureView = sampledView(baseColor_), .binding = 3u},
             {.textureView = sampledView(emissive_), .binding = 4u},
           },
           .storageTextures = {
             {.textureView = storageView(output_), .binding = 7u},
           },
         }}});
    shader_ = renderGraph->createShader(
        {.name = passName + "_Shader", .layout = layout_, .src = os::io::readRelativeFile("assets/shaders/spirv/svraster/sparse-voxel-compose.spirv"), .type = ::rendering::ShaderType::SpirV});
    pipeline_ = renderGraph->createComputePipeline(
        {.entry = "composeSparseVoxelShading", .layout = layout_, .name = passName + "_Pipeline", .shader = shader_});
  }

  ::rendering::Texture lighting_{};
  ::rendering::Texture baseColor_{};
  ::rendering::Texture emissive_{};
  ::rendering::Buffer coverage_{};
  ::rendering::Buffer linearDepth_{};
  ::rendering::Texture output_{};
  Settings settings_{};
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::Buffer rasterBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
