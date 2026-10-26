#include "atmosphere/rendering/AtmosphereLutPasses.hpp"

#include "atmosphere/rendering/AtmosphereGPU.hpp"
#include "globalIllumination/worldgi/WorldGITypes.hpp"
#include "os/File.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace atmosphere::rendering
{
namespace
{

::rendering::TextureView sampled(const ::rendering::Texture texture)
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

::rendering::TextureView sampledDepth(const ::rendering::Texture texture)
{
  return {.texture = texture,
          .index = 0u,
          .flags = ::rendering::ImageAspectFlags::Depth,
          .baseMipLevel = 0u,
          .levelCount = 1u,
          .baseArrayLayer = 0u,
          .layerCount = 1u,
          .access = ::rendering::AccessPattern::SHADER_READ,
          .layout = ::rendering::ResourceLayout::SHADER_READ_ONLY};
}

::rendering::TextureView storage(const ::rendering::Texture texture)
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

::rendering::Buffer createUniformBuffer(
    ::rendering::RenderGraph *graph, const std::string &name,
    const uint64_t size)
{
  return graph->createBuffer({
    .name = name,
    .size = size,
    .usage = static_cast<::rendering::BufferUsage>(
        ::rendering::BufferUsage::BufferUsage_Uniform |
        ::rendering::BufferUsage::BufferUsage_Push),
    // Camera-relative atmosphere passes are updated while the previous frame
    // may still be executing. Sharing one allocation lets the next frame's
    // matrices overwrite an in-flight aerial/fog dispatch, so generation,
    // reprojection, and composition can each observe a different camera.
    .frameLocal = true,
  });
}

void destroyCommon(::rendering::RenderGraph *graph,
    ::rendering::ComputePipeline &pipeline, ::rendering::Shader &shader,
    ::rendering::BindingGroups &groups, ::rendering::BindingsLayout &layout,
    ::rendering::Buffer &uniforms)
{
  if (pipeline.isValid()) graph->deleteComputePipeline(pipeline);
  if (shader.isValid()) graph->deleteShader(shader);
  if (groups.isValid()) graph->deleteBindingGroups(groups);
  if (layout.isValid()) graph->deleteBindingsLayout(layout);
  if (uniforms.isValid()) graph->deleteBuffer(uniforms);
}

::rendering::BindingGroupLayoutBufferEntry uniformLayoutEntry()
{
  return {.name = "uniforms",
          .binding = 0u,
          .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
          .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
          .isDynamic = false};
}

std::array<float, 4> terrainShadowControl(
    const AtmosphereTerrainShadowSampling &settings)
{
  const auto finitePositive = [](const float value, const float fallback)
  {
    return std::isfinite(value) ? std::max(value, 0.0f) : fallback;
  };
  return {settings.enabled ? 1.0f : 0.0f,
      finitePositive(settings.halfExtentMetres, 32'000.0f) * 0.001f,
      finitePositive(settings.halfDepthRangeMetres, 132'000.0f) * 0.001f,
      finitePositive(settings.depthBiasMetres, 12.0f) * 0.001f};
}

std::array<float, 4> terrainShadowFade(
    const AtmosphereTerrainShadowSampling &settings)
{
  return {
    std::clamp(std::isfinite(settings.minimumSunZenithCosine)
            ? settings.minimumSunZenithCosine : 0.04f,
        0.0f, 1.0f),
    std::clamp(std::isfinite(settings.fullSunZenithCosine)
            ? settings.fullSunZenithCosine : 0.12f,
        0.0f, 1.0f),
    std::clamp(std::isfinite(settings.edgeFadeStart)
            ? settings.edgeFadeStart : 0.82f,
        0.0f, 0.999f),
    std::clamp(std::isfinite(settings.strength)
            ? settings.strength : 1.0f,
        0.0f, 1.0f),
  };
}

} // namespace

struct alignas(16) TransmittanceUniformsGPU
{
  AtmosphereParametersGPU atmosphere{};
  std::array<uint32_t, 4> dispatch{};
};
static_assert(sizeof(TransmittanceUniformsGPU) == 144u);

AtmosphereTransmittancePass::AtmosphereTransmittancePass(
    const ::rendering::Texture output, Settings settings)
    : output_(output), settings_(settings)
{
}

AtmosphereTransmittancePass::~AtmosphereTransmittancePass()
{
  destroyCommon(renderGraph, pipeline_, shader_, groups_, layout_, uniforms_);
}

void AtmosphereTransmittancePass::update(Settings settings)
{
  settings_ = settings;
  if (uniforms_.isValid()) upload();
}

void AtmosphereTransmittancePass::recordCommandBuffer(
    ::rendering::CommandRecorder &commands)
{
  ensureResources();
  upload();
  commands.cmdBindComputePipeline(pipeline_);
  commands.cmdBindBindingGroups(groups_, nullptr, 0u);
  commands.cmdDispatch((std::max(settings_.width, 1u) + 7u) / 8u,
      (std::max(settings_.height, 1u) + 7u) / 8u, 1u);
}

void AtmosphereTransmittancePass::upload()
{
  const TransmittanceUniformsGPU values{
    makeAtmosphereParametersGPU(settings_.profile, settings_.sun),
    {std::max(settings_.width, 1u), std::max(settings_.height, 1u),
     std::clamp(settings_.integrationSteps, 1u, 256u),
     settings_.rebuild ? 1u : 0u},
  };
  renderGraph->bufferWrite(uniforms_, 0u, sizeof(values),
      const_cast<TransmittanceUniformsGPU *>(&values));
}

void AtmosphereTransmittancePass::ensureResources()
{
  if (layout_.isValid()) return;
  uniforms_ = createUniformBuffer(
      renderGraph, passName + "_Uniforms", sizeof(TransmittanceUniformsGPU));
  layout_ = renderGraph->createBindingsLayout({
    .name = passName + "_Layout",
    .groups = {::rendering::BindingGroupLayout{
      .buffers = {uniformLayoutEntry()},
      .storageTextures = {
        {.name = "outputLut", .binding = 1u,
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
           .size = sizeof(TransmittanceUniformsGPU),
           .access = ::rendering::AccessPattern::UNIFORM_READ},
         .binding = 0u},
      },
      .storageTextures = {{.textureView = storage(output_), .binding = 1u}},
    }},
  });
  shader_ = renderGraph->createShader({
    .name = passName + "_Shader",
    .src = os::io::readRelativeFile(
        "assets/shaders/spirv/atmosphere/atmosphere-transmittance.spirv"),
    .type = ::rendering::ShaderType::SpirV,
    .layout = layout_,
  });
  pipeline_ = renderGraph->createComputePipeline({
    .name = passName + "_Pipeline",
    .shader = shader_,
    .entry = "buildAtmosphereTransmittance",
    .layout = layout_,
  });
}

struct alignas(16) MultipleScatteringUniformsGPU
{
  AtmosphereParametersGPU atmosphere{};
  std::array<uint32_t, 4> dispatch{};
  std::array<uint32_t, 4> integration{};
};
static_assert(sizeof(MultipleScatteringUniformsGPU) == 160u);

AtmosphereMultipleScatteringPass::AtmosphereMultipleScatteringPass(
    const ::rendering::Texture transmittance,
    const ::rendering::Texture output, Settings settings)
    : transmittance_(transmittance), output_(output), settings_(settings)
{
}

AtmosphereMultipleScatteringPass::~AtmosphereMultipleScatteringPass()
{
  destroyCommon(renderGraph, pipeline_, shader_, groups_, layout_, uniforms_);
}

void AtmosphereMultipleScatteringPass::update(Settings settings)
{
  settings_ = settings;
  if (uniforms_.isValid()) upload();
}

void AtmosphereMultipleScatteringPass::recordCommandBuffer(
    ::rendering::CommandRecorder &commands)
{
  ensureResources();
  upload();
  commands.cmdBindComputePipeline(pipeline_);
  commands.cmdBindBindingGroups(groups_, nullptr, 0u);
  commands.cmdDispatch((std::max(settings_.width, 1u) + 7u) / 8u,
      (std::max(settings_.height, 1u) + 7u) / 8u, 1u);
}

void AtmosphereMultipleScatteringPass::upload()
{
  const MultipleScatteringUniformsGPU values{
    makeAtmosphereParametersGPU(settings_.profile, settings_.sun),
    {std::max(settings_.width, 1u), std::max(settings_.height, 1u),
     std::clamp(settings_.directionCount, 1u, 64u),
     settings_.rebuild ? 1u : 0u},
    {std::clamp(settings_.integrationSteps, 1u, 64u), 0u, 0u, 0u},
  };
  renderGraph->bufferWrite(uniforms_, 0u, sizeof(values),
      const_cast<MultipleScatteringUniformsGPU *>(&values));
}

void AtmosphereMultipleScatteringPass::ensureResources()
{
  if (layout_.isValid()) return;
  uniforms_ = createUniformBuffer(renderGraph, passName + "_Uniforms",
      sizeof(MultipleScatteringUniformsGPU));
  layout_ = renderGraph->createBindingsLayout({
    .name = passName + "_Layout",
    .groups = {::rendering::BindingGroupLayout{
      .buffers = {uniformLayoutEntry()},
      .textures = {
        {.name = "transmittanceLut", .binding = 1u,
         .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
      },
      .storageTextures = {
        {.name = "outputLut", .binding = 2u,
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
           .size = sizeof(MultipleScatteringUniformsGPU),
           .access = ::rendering::AccessPattern::UNIFORM_READ},
         .binding = 0u},
      },
      .textures = {{.textureView = sampled(transmittance_), .binding = 1u}},
      .storageTextures = {{.textureView = storage(output_), .binding = 2u}},
    }},
  });
  shader_ = renderGraph->createShader({
    .name = passName + "_Shader",
    .src = os::io::readRelativeFile(
        "assets/shaders/spirv/atmosphere/atmosphere-multiple-scattering.spirv"),
    .type = ::rendering::ShaderType::SpirV,
    .layout = layout_,
  });
  pipeline_ = renderGraph->createComputePipeline({
    .name = passName + "_Pipeline",
    .shader = shader_,
    .entry = "buildAtmosphereMultipleScattering",
    .layout = layout_,
  });
}

struct alignas(16) SkyViewUniformsGPU
{
  AtmosphereParametersGPU atmosphere{};
  std::array<uint32_t, 4> dispatch{};
  std::array<float, 4> cameraSun{};
  std::array<float, 4> localUp{};
  std::array<float, 4> terrainShadowControl{};
  std::array<float, 4> terrainShadowFade{};
};
static_assert(sizeof(SkyViewUniformsGPU) == 208u);

AtmosphereSkyViewPass::AtmosphereSkyViewPass(
    const ::rendering::Texture transmittance,
    const ::rendering::Texture multipleScattering,
    const ::rendering::Texture terrainShadow,
    const ::rendering::Texture output, Settings settings)
    : transmittance_(transmittance),
      multipleScattering_(multipleScattering), terrainShadow_(terrainShadow),
      output_(output),
      settings_(settings)
{
}

AtmosphereSkyViewPass::~AtmosphereSkyViewPass()
{
  destroyCommon(renderGraph, pipeline_, shader_, groups_, layout_, uniforms_);
}

void AtmosphereSkyViewPass::update(Settings settings)
{
  settings_ = settings;
  if (uniforms_.isValid()) upload();
}

void AtmosphereSkyViewPass::recordCommandBuffer(
    ::rendering::CommandRecorder &commands)
{
  ensureResources();
  upload();
  commands.cmdBindComputePipeline(pipeline_);
  commands.cmdBindBindingGroups(groups_, nullptr, 0u);
  commands.cmdDispatch((std::max(settings_.width, 1u) + 7u) / 8u,
      (std::max(settings_.height, 1u) + 7u) / 8u, 1u);
}

void AtmosphereSkyViewPass::upload()
{
  const float atmosphereHeight = static_cast<float>(
      settings_.profile.topRadiusMetres - settings_.profile.groundRadiusMetres);
  const float altitude = std::clamp(
      std::isfinite(settings_.cameraAltitudeMetres)
          ? settings_.cameraAltitudeMetres : 0.0f,
      0.0f, std::max(atmosphereHeight, 0.0f));
  std::array<float, 3> up = settings_.localUpWorld;
  const float upLengthSquared = up[0] * up[0] + up[1] * up[1] + up[2] * up[2];
  if (!std::isfinite(upLengthSquared) || upLengthSquared <= 1.0e-12f)
    up = {0.0f, 1.0f, 0.0f};
  else
  {
    const float inverseLength = 1.0f / std::sqrt(upLengthSquared);
    for (float &component : up) component *= inverseLength;
  }
  const SkyViewUniformsGPU values{
    makeAtmosphereParametersGPU(settings_.profile, settings_.sun),
    {std::max(settings_.width, 1u), std::max(settings_.height, 1u),
     std::clamp(settings_.integrationSteps, 1u, 64u),
     settings_.enabled ? 1u : 0u},
    {static_cast<float>(settings_.profile.groundRadiusMetres) * 0.001f +
     altitude * 0.001f,
     std::clamp(settings_.sunZenithCosine, -1.0f, 1.0f), 0.0f, 0.0f},
    {up[0], up[1], up[2], 0.0f},
    terrainShadowControl(settings_.terrainShadow),
    terrainShadowFade(settings_.terrainShadow),
  };
  renderGraph->bufferWrite(uniforms_, 0u, sizeof(values),
      const_cast<SkyViewUniformsGPU *>(&values));
}

void AtmosphereSkyViewPass::ensureResources()
{
  if (layout_.isValid()) return;
  uniforms_ = createUniformBuffer(
      renderGraph, passName + "_Uniforms", sizeof(SkyViewUniformsGPU));
  layout_ = renderGraph->createBindingsLayout({
    .name = passName + "_Layout",
    .groups = {::rendering::BindingGroupLayout{
      .buffers = {uniformLayoutEntry()},
      .textures = {
        {.name = "transmittanceLut", .binding = 1u,
         .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "multipleScatteringLut", .binding = 2u,
         .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "terrainShadowMap", .binding = 4u,
         .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
      },
      .storageTextures = {
        {.name = "outputLut", .binding = 3u,
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
           .size = sizeof(SkyViewUniformsGPU),
           .access = ::rendering::AccessPattern::UNIFORM_READ},
         .binding = 0u},
      },
      .textures = {
        {.textureView = sampled(transmittance_), .binding = 1u},
        {.textureView = sampled(multipleScattering_), .binding = 2u},
        {.textureView = sampledDepth(terrainShadow_), .binding = 4u},
      },
      .storageTextures = {{.textureView = storage(output_), .binding = 3u}},
    }},
  });
  shader_ = renderGraph->createShader({
    .name = passName + "_Shader",
    .src = os::io::readRelativeFile(
        "assets/shaders/spirv/atmosphere/atmosphere-sky-view.spirv"),
    .type = ::rendering::ShaderType::SpirV,
    .layout = layout_,
  });
  pipeline_ = renderGraph->createComputePipeline({
    .name = passName + "_Pipeline",
    .shader = shader_,
    .entry = "buildAtmosphereSkyView",
    .layout = layout_,
  });
}

struct alignas(16) AerialPerspectiveUniformsGPU
{
  AtmosphereParametersGPU atmosphere{};
  std::array<uint32_t, 4> dispatch{};
  std::array<uint32_t, 4> viewport{};
  std::array<float, 4> cameraDistance{};
  std::array<float, 4> localUp{};
  std::array<float, 16> cameraToWorld{};
  std::array<float, 4> focalPrincipal{};
  std::array<float, 4> terrainShadowControl{};
  std::array<float, 4> terrainShadowFade{};
  // x ground extinction / km, y height falloff km, z cutoff km, w enabled.
  std::array<float, 4> volumetricFogControl{};
  // rgb single-scattering albedo, w Henyey-Greenstein anisotropy.
  std::array<float, 4> volumetricFogAlbedoPhase{};
  std::array<float, 4> volumetricFogDistance{};
};
static_assert(sizeof(AerialPerspectiveUniformsGPU) == 352u);

AtmosphereAerialPerspectivePass::AtmosphereAerialPerspectivePass(
    const ::rendering::Texture transmittance,
    const ::rendering::Texture multipleScattering,
    const ::rendering::Texture terrainShadow,
    const ::rendering::Texture output, Settings settings)
    : transmittance_(transmittance),
      multipleScattering_(multipleScattering), terrainShadow_(terrainShadow),
      output_(output),
      settings_(settings)
{
}

AtmosphereAerialPerspectivePass::~AtmosphereAerialPerspectivePass()
{
  destroyCommon(renderGraph, pipeline_, shader_, groups_, layout_, uniforms_);
}

void AtmosphereAerialPerspectivePass::update(Settings settings)
{
  settings_ = settings;
  if (uniforms_.isValid()) upload();
}

void AtmosphereAerialPerspectivePass::recordCommandBuffer(
    ::rendering::CommandRecorder &commands)
{
  ensureResources();
  upload();
  commands.cmdBindComputePipeline(pipeline_);
  commands.cmdBindBindingGroups(groups_, nullptr, 0u);
  // The shader owns an entire ordered z column per invocation. This avoids
  // independently re-integrating the prefix of the ray for every endpoint.
  commands.cmdDispatch((std::max(settings_.width, 1u) + 7u) / 8u,
      (std::max(settings_.height, 1u) + 7u) / 8u, 1u);
}

void AtmosphereAerialPerspectivePass::upload()
{
  const float atmosphereHeight = static_cast<float>(
      settings_.profile.topRadiusMetres - settings_.profile.groundRadiusMetres);
  const float altitude = std::clamp(
      std::isfinite(settings_.cameraAltitudeMetres)
          ? settings_.cameraAltitudeMetres : 0.0f,
      0.0f, std::max(atmosphereHeight, 0.0f));
  std::array<float, 3> up = settings_.localUpWorld;
  const float upLengthSquared = up[0] * up[0] + up[1] * up[1] + up[2] * up[2];
  if (!std::isfinite(upLengthSquared) || upLengthSquared <= 1.0e-12f)
    up = {0.0f, 1.0f, 0.0f};
  else
  {
    const float inverseLength = 1.0f / std::sqrt(upLengthSquared);
    for (float &component : up) component *= inverseLength;
  }
  const AerialPerspectiveUniformsGPU values{
    makeAtmosphereParametersGPU(settings_.profile, settings_.sun),
    {std::max(settings_.width, 1u), std::max(settings_.height, 1u),
     std::max(settings_.depth, 1u),
     std::clamp(settings_.integrationSteps, 1u, 4u)},
    {std::max(settings_.viewportWidth, 1u),
     std::max(settings_.viewportHeight, 1u), settings_.enabled ? 1u : 0u, 0u},
    {static_cast<float>(settings_.profile.groundRadiusMetres) * 0.001f +
         altitude * 0.001f,
     std::max(std::isfinite(settings_.maximumDistanceMetres)
                  ? settings_.maximumDistanceMetres : 0.0f,
         1.0f) * 0.001f,
     std::max(std::isfinite(settings_.distributionScaleMetres)
                  ? settings_.distributionScaleMetres : 0.0f,
         0.001f) * 0.001f,
     static_cast<float>(settings_.frameIndex & 1023u)},
    {up[0], up[1], up[2], 0.0f},
    settings_.cameraToWorld,
    settings_.focalPrincipal,
    terrainShadowControl(settings_.terrainShadow),
    terrainShadowFade(settings_.terrainShadow),
    {std::max(std::isfinite(settings_.volumetricFogDensityPerMetre)
                  ? settings_.volumetricFogDensityPerMetre : 0.0f,
         0.0f) * 1000.0f,
     std::max(std::isfinite(settings_.volumetricFogHeightFalloffMetres)
                  ? settings_.volumetricFogHeightFalloffMetres : 0.0f,
         1.0f) * 0.001f,
     std::max(std::isfinite(settings_.volumetricFogMaximumHeightMetres)
                  ? settings_.volumetricFogMaximumHeightMetres : 0.0f,
         1.0f) * 0.001f,
     settings_.volumetricFogEnabled ? 1.0f : 0.0f},
    {std::clamp(settings_.volumetricFogAlbedo[0], 0.0f, 1.0f),
     std::clamp(settings_.volumetricFogAlbedo[1], 0.0f, 1.0f),
     std::clamp(settings_.volumetricFogAlbedo[2], 0.0f, 1.0f),
     std::clamp(settings_.volumetricFogAnisotropy, -0.95f, 0.95f)},
    {std::max(std::isfinite(settings_.volumetricFogFullDistanceMetres)
                  ? settings_.volumetricFogFullDistanceMetres : 0.0f,
         0.0f) * 0.001f,
     std::max(std::isfinite(settings_.volumetricFogFadeEndDistanceMetres)
                  ? settings_.volumetricFogFadeEndDistanceMetres : 0.0f,
         1.0f) * 0.001f,
     0.0f, 0.0f},
  };
  renderGraph->bufferWrite(uniforms_, 0u, sizeof(values),
      const_cast<AerialPerspectiveUniformsGPU *>(&values));
}

void AtmosphereAerialPerspectivePass::ensureResources()
{
  if (layout_.isValid()) return;
  uniforms_ = createUniformBuffer(renderGraph, passName + "_Uniforms",
      sizeof(AerialPerspectiveUniformsGPU));
  layout_ = renderGraph->createBindingsLayout({
    .name = passName + "_Layout",
    .groups = {::rendering::BindingGroupLayout{
      .buffers = {uniformLayoutEntry()},
      .textures = {
        {.name = "transmittanceLut", .binding = 1u,
         .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "multipleScatteringLut", .binding = 2u,
         .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "terrainShadowMap", .binding = 4u,
         .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
      },
      .storageTextures = {
        {.name = "outputLut", .binding = 3u,
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
           .size = sizeof(AerialPerspectiveUniformsGPU),
           .access = ::rendering::AccessPattern::UNIFORM_READ},
         .binding = 0u},
      },
      .textures = {
        {.textureView = sampled(transmittance_), .binding = 1u},
        {.textureView = sampled(multipleScattering_), .binding = 2u},
        {.textureView = sampledDepth(terrainShadow_), .binding = 4u},
      },
      .storageTextures = {{.textureView = storage(output_), .binding = 3u}},
    }},
  });
  shader_ = renderGraph->createShader({
    .name = passName + "_Shader",
    .src = os::io::readRelativeFile(
        "assets/shaders/spirv/atmosphere/atmosphere-aerial-perspective.spirv"),
    .type = ::rendering::ShaderType::SpirV,
    .layout = layout_,
  });
  pipeline_ = renderGraph->createComputePipeline({
    .name = passName + "_Pipeline",
    .shader = shader_,
    .entry = "buildAtmosphereAerialPerspective",
    .layout = layout_,
  });
}

struct alignas(16) AerialTemporalUniformsGPU
{
  std::array<uint32_t, 4> dispatchHistory{};
  std::array<uint32_t, 4> viewport{};
  std::array<float, 4> depthHistory{};
  std::array<float, 4> currentCameraPosition{};
  std::array<float, 4> previousCameraPosition{};
  std::array<float, 16> currentCameraToWorld{};
  std::array<float, 16> previousWorldToCamera{};
  std::array<float, 4> currentFocalPrincipal{};
  std::array<float, 4> previousFocalPrincipal{};
};
static_assert(sizeof(AerialTemporalUniformsGPU) == 240u);

AtmosphereAerialTemporalPass::AtmosphereAerialTemporalPass(
    const ::rendering::Texture current, const ::rendering::Texture history,
    const ::rendering::Texture output, Settings settings)
    : current_(current), history_(history), output_(output),
      settings_(settings)
{
}

AtmosphereAerialTemporalPass::~AtmosphereAerialTemporalPass()
{
  destroyCommon(renderGraph, pipeline_, shader_, groups_, layout_, uniforms_);
  if (sampler_.isValid()) renderGraph->deleteSampler(sampler_);
}

void AtmosphereAerialTemporalPass::update(Settings settings)
{
  settings_ = settings;
  if (uniforms_.isValid()) upload();
}

void AtmosphereAerialTemporalPass::recordCommandBuffer(
    ::rendering::CommandRecorder &commands)
{
  ensureResources();
  upload();
  commands.cmdBindComputePipeline(pipeline_);
  commands.cmdBindBindingGroups(groups_, nullptr, 0u);
  commands.cmdDispatch((std::max(settings_.width, 1u) + 3u) / 4u,
      (std::max(settings_.height, 1u) + 3u) / 4u,
      (std::max(settings_.depth, 1u) + 3u) / 4u);
}

void AtmosphereAerialTemporalPass::upload()
{
  const auto finite = [](const float value, const float fallback)
  {
    return std::isfinite(value) ? value : fallback;
  };
  const AerialTemporalUniformsGPU values{
    {std::max(settings_.width, 1u), std::max(settings_.height, 1u),
     std::max(settings_.depth, 1u), settings_.historyValid ? 1u : 0u},
    {std::max(settings_.viewportWidth, 1u),
     std::max(settings_.viewportHeight, 1u),
     std::max(settings_.previousViewportWidth, 1u),
     std::max(settings_.previousViewportHeight, 1u)},
    {std::max(finite(settings_.maximumDistanceMetres, 1.0f), 1.0f) * 0.001f,
     std::max(finite(settings_.distributionScaleMetres, 0.001f), 0.001f) *
         0.001f,
     std::clamp(finite(settings_.historyWeight, 0.0f), 0.0f, 0.95f),
     static_cast<float>(settings_.frameIndex & 1023u)},
    {finite(settings_.cameraWorldPositionMetres[0], 0.0f),
     finite(settings_.cameraWorldPositionMetres[1], 0.0f),
     finite(settings_.cameraWorldPositionMetres[2], 0.0f), 0.0f},
    {finite(settings_.previousCameraWorldPositionMetres[0], 0.0f),
     finite(settings_.previousCameraWorldPositionMetres[1], 0.0f),
     finite(settings_.previousCameraWorldPositionMetres[2], 0.0f), 0.0f},
    settings_.cameraToWorld,
    settings_.previousWorldToCamera,
    settings_.focalPrincipal,
    settings_.previousFocalPrincipal,
  };
  renderGraph->bufferWrite(uniforms_, 0u, sizeof(values),
      const_cast<AerialTemporalUniformsGPU *>(&values));
}

void AtmosphereAerialTemporalPass::ensureResources()
{
  if (layout_.isValid()) return;
  uniforms_ = createUniformBuffer(renderGraph, passName + "_Uniforms",
      sizeof(AerialTemporalUniformsGPU));
  layout_ = renderGraph->createBindingsLayout({
    .name = passName + "_Layout",
    .groups = {::rendering::BindingGroupLayout{
      .buffers = {uniformLayoutEntry()},
      .samplers = {
        {.name = "volumeSampler", .binding = 3u,
         .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
      },
      .textures = {
        {.name = "currentVolume", .binding = 1u,
         .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "historyVolume", .binding = 2u,
         .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
      },
      .storageTextures = {
        {.name = "outputVolume", .binding = 4u,
         .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
      },
    }},
  });
  sampler_ = renderGraph->createSampler({
    .name = passName + "_Sampler",
    .minFilter = ::rendering::Filter::Linear,
    .magFilter = ::rendering::Filter::Linear,
    .addressModeU = ::rendering::SamplerAddressMode::ClampToEdge,
    .addressModeV = ::rendering::SamplerAddressMode::ClampToEdge,
    .addressModeW = ::rendering::SamplerAddressMode::ClampToEdge,
    .anisotropyEnable = false,
    .maxAnisotropy = 1.0f,
    .maxLod = 0.0f,
  });
  groups_ = renderGraph->createBindingGroups({
    .name = passName + "_Groups",
    .layout = layout_,
    .groups = {::rendering::GroupInfo{
      .name = "group0",
      .buffers = {
        {.bufferView = {.buffer = uniforms_, .offset = 0u,
           .size = sizeof(AerialTemporalUniformsGPU),
           .access = ::rendering::AccessPattern::UNIFORM_READ},
         .binding = 0u},
      },
      .samplers = {
        {.binding = 3u, .sampler = sampler_, .view = sampled(history_)},
      },
      .textures = {
        {.textureView = sampled(current_), .binding = 1u},
        {.textureView = sampled(history_), .binding = 2u},
      },
      .storageTextures = {
        {.textureView = storage(output_), .binding = 4u},
      },
    }},
  });
  shader_ = renderGraph->createShader({
    .name = passName + "_Shader",
    .src = os::io::readRelativeFile(
        "assets/shaders/spirv/atmosphere/atmosphere-aerial-temporal.spirv"),
    .type = ::rendering::ShaderType::SpirV,
    .layout = layout_,
  });
  pipeline_ = renderGraph->createComputePipeline({
    .name = passName + "_Pipeline",
    .shader = shader_,
    .entry = "resolveAtmosphereAerialTemporal",
    .layout = layout_,
  });
}

struct alignas(16) AmbientUniformsGPU
{
  std::array<float, 4> localUp{};
  std::array<float, 4> directionTowardSun{};
  std::array<uint32_t, 4> dispatch{};
};
static_assert(sizeof(AmbientUniformsGPU) == 48u);

AtmosphereAmbientPass::AtmosphereAmbientPass(
    const ::rendering::Texture skyView,
    const ::rendering::Buffer sceneLights, Settings settings)
    : skyView_(skyView), sceneLights_(sceneLights), settings_(settings)
{
}

AtmosphereAmbientPass::~AtmosphereAmbientPass()
{
  destroyCommon(renderGraph, pipeline_, shader_, groups_, layout_, uniforms_);
}

void AtmosphereAmbientPass::update(Settings settings)
{
  settings_ = settings;
  if (uniforms_.isValid()) upload();
}

void AtmosphereAmbientPass::recordCommandBuffer(
    ::rendering::CommandRecorder &commands)
{
  ensureResources();
  upload();
  commands.cmdBindComputePipeline(pipeline_);
  commands.cmdBindBindingGroups(groups_, nullptr, 0u);
  commands.cmdDispatch(1u, 1u, 1u);
}

void AtmosphereAmbientPass::upload()
{
  const auto normalize = [](std::array<float, 3> value,
                             const std::array<float, 3> &fallback)
  {
    const float lengthSquared = value[0] * value[0] +
        value[1] * value[1] + value[2] * value[2];
    if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-12f)
      return fallback;
    const float inverseLength = 1.0f / std::sqrt(lengthSquared);
    for (float &component : value) component *= inverseLength;
    return value;
  };
  const auto up = normalize(settings_.localUpWorld, {0.0f, 1.0f, 0.0f});
  const auto sun = normalize(
      settings_.directionTowardSunWorld, up);
  const AmbientUniformsGPU values{
    {up[0], up[1], up[2],
     std::max(settings_.nightAmbientScale, 0.0f)},
    {sun[0], sun[1], sun[2],
     std::max(settings_.ambientIntensity, 0.0f)},
    {std::clamp(settings_.sampleCount, 1u, 64u),
     settings_.skyLightIndex, std::max(settings_.lightCapacity, 1u),
     settings_.enabled ? 1u : 0u},
  };
  renderGraph->bufferWrite(uniforms_, 0u, sizeof(values),
      const_cast<AmbientUniformsGPU *>(&values));
}

void AtmosphereAmbientPass::ensureResources()
{
  if (layout_.isValid()) return;
  uniforms_ = createUniformBuffer(
      renderGraph, passName + "_Uniforms", sizeof(AmbientUniformsGPU));
  layout_ = renderGraph->createBindingsLayout({
    .name = passName + "_Layout",
    .groups = {::rendering::BindingGroupLayout{
      .buffers = {
        uniformLayoutEntry(),
        {.name = "sceneLights", .binding = 2u,
         .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
         .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
         .isDynamic = false},
      },
      .textures = {
        {.name = "skyViewLut", .binding = 1u,
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
           .size = sizeof(AmbientUniformsGPU),
           .access = ::rendering::AccessPattern::UNIFORM_READ},
         .binding = 0u},
        {.bufferView = {.buffer = sceneLights_, .offset = 0u,
           .size = globalillumination::worldgi::worldGILightBufferSize(
               std::max(settings_.lightCapacity, 1u)),
           .access = ::rendering::AccessPattern::SHADER_READ_WRITE},
         .binding = 2u},
      },
      .textures = {{.textureView = sampled(skyView_), .binding = 1u}},
    }},
  });
  shader_ = renderGraph->createShader({
    .name = passName + "_Shader",
    .src = os::io::readRelativeFile(
        "assets/shaders/spirv/atmosphere/atmosphere-ambient.spirv"),
    .type = ::rendering::ShaderType::SpirV,
    .layout = layout_,
  });
  pipeline_ = renderGraph->createComputePipeline({
    .name = passName + "_Pipeline",
    .shader = shader_,
    .entry = "projectAtmosphereAmbient",
    .layout = layout_,
  });
}

} // namespace atmosphere::rendering
