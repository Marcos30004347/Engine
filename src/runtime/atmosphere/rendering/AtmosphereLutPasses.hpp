#pragma once

#include "atmosphere/AtmosphereTypes.hpp"
#include "rendering/gpu/RenderGraph.hpp"

#include <array>
#include <cstdint>

namespace atmosphere::rendering
{

struct AtmosphereLutQuality
{
  uint32_t transmittanceWidth = 256u;
  uint32_t transmittanceHeight = 64u;
  uint32_t transmittanceSteps = 40u;
  uint32_t multipleScatteringWidth = 32u;
  uint32_t multipleScatteringHeight = 32u;
  uint32_t multipleScatteringDirections = 64u;
  uint32_t multipleScatteringSteps = 20u;
  uint32_t skyViewWidth = 200u;
  uint32_t skyViewHeight = 100u;
  uint32_t skyViewSteps = 28u;
  uint32_t aerialPerspectiveWidth = 32u;
  uint32_t aerialPerspectiveHeight = 32u;
  uint32_t aerialPerspectiveDepth = 32u;
  uint32_t aerialPerspectiveSteps = 12u;
  uint32_t ambientDirections = 64u;
};

struct AtmosphereTerrainShadowSampling
{
  bool enabled = false;
  float halfExtentMetres = 32'000.0f;
  float halfDepthRangeMetres = 132'000.0f;
  float depthBiasMetres = 12.0f;
  float minimumSunZenithCosine = 0.04f;
  float fullSunZenithCosine = 0.12f;
  float edgeFadeStart = 0.82f;
  float strength = 1.0f;
};

class AtmosphereTransmittancePass final : public ::rendering::Pass
{
public:
  struct Settings
  {
    AtmosphereProfile profile{};
    SunSettings sun{};
    uint32_t width = 256u;
    uint32_t height = 64u;
    uint32_t integrationSteps = 40u;
    bool rebuild = true;
  };

  AtmosphereTransmittancePass(::rendering::Texture output, Settings settings);
  ~AtmosphereTransmittancePass() override;
  void update(Settings settings);
  void recordCommandBuffer(::rendering::CommandRecorder &commands) override;

private:
  void ensureResources();
  void upload();
  ::rendering::Texture output_{};
  Settings settings_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

class AtmosphereMultipleScatteringPass final : public ::rendering::Pass
{
public:
  struct Settings
  {
    AtmosphereProfile profile{};
    SunSettings sun{};
    uint32_t width = 32u;
    uint32_t height = 32u;
    uint32_t directionCount = 64u;
    uint32_t integrationSteps = 20u;
    bool rebuild = true;
  };

  AtmosphereMultipleScatteringPass(
      ::rendering::Texture transmittance, ::rendering::Texture output,
      Settings settings);
  ~AtmosphereMultipleScatteringPass() override;
  void update(Settings settings);
  void recordCommandBuffer(::rendering::CommandRecorder &commands) override;

private:
  void ensureResources();
  void upload();
  ::rendering::Texture transmittance_{};
  ::rendering::Texture output_{};
  Settings settings_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

class AtmosphereSkyViewPass final : public ::rendering::Pass
{
public:
  struct Settings
  {
    AtmosphereProfile profile{};
    SunSettings sun{};
    // Camera altitude above the ground reference sphere.
    float cameraAltitudeMetres = 2.0f;
    // Dot(local up, direction toward the sun), not direction light travels.
    float sunZenithCosine = 1.0f;
    std::array<float, 3> localUpWorld{0.0f, 1.0f, 0.0f};
    AtmosphereTerrainShadowSampling terrainShadow{};
    uint32_t width = 200u;
    uint32_t height = 100u;
    uint32_t integrationSteps = 28u;
    bool enabled = true;
  };

  AtmosphereSkyViewPass(::rendering::Texture transmittance,
      ::rendering::Texture multipleScattering,
      ::rendering::Texture terrainShadow, ::rendering::Texture output,
      Settings settings);
  ~AtmosphereSkyViewPass() override;
  void update(Settings settings);
  void recordCommandBuffer(::rendering::CommandRecorder &commands) override;

private:
  void ensureResources();
  void upload();
  ::rendering::Texture transmittance_{};
  ::rendering::Texture multipleScattering_{};
  ::rendering::Texture terrainShadow_{};
  ::rendering::Texture output_{};
  Settings settings_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

class AtmosphereAerialPerspectivePass final : public ::rendering::Pass
{
public:
  struct Settings
  {
    AtmosphereProfile profile{};
    SunSettings sun{};
    float cameraAltitudeMetres = 2.0f;
    float maximumDistanceMetres = 32'000.0f;
    float distributionScaleMetres = 100.0f;
    // Low-cost local participating medium integrated into the existing
    // logarithmically distributed froxel volume. Density is extinction per
    // metre at ground level; height falloff and cutoff are metres.
    bool volumetricFogEnabled = false;
    float volumetricFogDensityPerMetre = 0.0f;
    float volumetricFogHeightFalloffMetres = 180.0f;
    float volumetricFogMaximumHeightMetres = 2'000.0f;
    float volumetricFogFullDistanceMetres = 3'000.0f;
    float volumetricFogFadeEndDistanceMetres = 10'000.0f;
    std::array<float, 3> volumetricFogAlbedo{0.92f, 0.95f, 1.0f};
    float volumetricFogAnisotropy = 0.60f;
    std::array<float, 3> localUpWorld{0.0f, 1.0f, 0.0f};
    std::array<float, 16> cameraToWorld{};
    std::array<float, 4> focalPrincipal{};
    AtmosphereTerrainShadowSampling terrainShadow{};
    uint32_t viewportWidth = 1u;
    uint32_t viewportHeight = 1u;
    uint32_t width = 32u;
    uint32_t height = 32u;
    uint32_t depth = 32u;
    uint32_t integrationSteps = 12u;
    // Rotates the sub-froxel integration position. The final temporal resolve
    // accumulates these low-discrepancy samples into smooth shafts.
    uint32_t frameIndex = 0u;
    bool enabled = true;
  };

  AtmosphereAerialPerspectivePass(::rendering::Texture transmittance,
      ::rendering::Texture multipleScattering,
      ::rendering::Texture terrainShadow, ::rendering::Texture output,
      Settings settings);
  ~AtmosphereAerialPerspectivePass() override;
  void update(Settings settings);
  void recordCommandBuffer(::rendering::CommandRecorder &commands) override;

private:
  void ensureResources();
  void upload();
  ::rendering::Texture transmittance_{};
  ::rendering::Texture multipleScattering_{};
  ::rendering::Texture terrainShadow_{};
  ::rendering::Texture output_{};
  Settings settings_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

// Reprojects the integrated froxel volume into the previous camera and applies
// a neighborhood-clipped temporal accumulation. This is deliberately separate
// from medium integration: current lighting remains available for rejection,
// and history never feeds back into physical transmittance calculations.
class AtmosphereAerialTemporalPass final : public ::rendering::Pass
{
public:
  struct Settings
  {
    uint32_t width = 1u;
    uint32_t height = 1u;
    uint32_t depth = 1u;
    uint32_t viewportWidth = 1u;
    uint32_t viewportHeight = 1u;
    uint32_t previousViewportWidth = 1u;
    uint32_t previousViewportHeight = 1u;
    float maximumDistanceMetres = 32'000.0f;
    float distributionScaleMetres = 100.0f;
    float historyWeight = 0.88f;
    uint32_t frameIndex = 0u;
    bool historyValid = false;
    std::array<float, 3> cameraWorldPositionMetres{};
    std::array<float, 3> previousCameraWorldPositionMetres{};
    std::array<float, 16> cameraToWorld{};
    std::array<float, 16> previousWorldToCamera{};
    std::array<float, 4> focalPrincipal{};
    std::array<float, 4> previousFocalPrincipal{};
  };

  AtmosphereAerialTemporalPass(::rendering::Texture current,
      ::rendering::Texture history, ::rendering::Texture output,
      Settings settings);
  ~AtmosphereAerialTemporalPass() override;
  void update(Settings settings);
  void recordCommandBuffer(::rendering::CommandRecorder &commands) override;

private:
  void ensureResources();
  void upload();
  ::rendering::Texture current_{};
  ::rendering::Texture history_{};
  ::rendering::Texture output_{};
  Settings settings_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Sampler sampler_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

// Projects the physical Sky-View LUT into the sky WorldGILight record. One
// invocation integrates a fixed spherical sample set and publishes four RGB
// first-order SH coefficients for every voxel lighting consumer.
class AtmosphereAmbientPass final : public ::rendering::Pass
{
public:
  struct Settings
  {
    std::array<float, 3> localUpWorld{0.0f, 1.0f, 0.0f};
    std::array<float, 3> directionTowardSunWorld{0.0f, 1.0f, 0.0f};
    float nightAmbientScale = 0.02f;
    // Multiplier applied after projecting the physical Sky-View LUT. The LUT
    // already contains sun intensity and the atmosphere radiance scale, so
    // this is an artistic ambient/direct balance rather than another exposure.
    float ambientIntensity = 1.0f;
    uint32_t sampleCount = 64u;
    uint32_t skyLightIndex = 1u;
    uint32_t lightCapacity = 8u;
    bool enabled = true;
  };

  AtmosphereAmbientPass(::rendering::Texture skyView,
      ::rendering::Buffer sceneLights, Settings settings);
  ~AtmosphereAmbientPass() override;
  void update(Settings settings);
  void recordCommandBuffer(::rendering::CommandRecorder &commands) override;

private:
  void ensureResources();
  void upload();
  ::rendering::Texture skyView_{};
  ::rendering::Buffer sceneLights_{};
  Settings settings_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace atmosphere::rendering
