#pragma once

#include "atmosphere/AtmosphereTypes.hpp"

#include <array>
#include <cstdint>

namespace atmosphere::rendering
{

// Shader-space lengths are kilometres. This keeps an Earth-sized radius and
// metre-scale density changes representable in FP32 without baking a planet
// scale into any shader.
struct alignas(16) AtmosphereParametersGPU
{
  std::array<float, 4> radiiMieG{};
  std::array<float, 4> rayleighScatteringScale{};
  std::array<float, 4> mieScatteringScale{};
  std::array<float, 4> mieAbsorptionOzoneCentre{};
  std::array<float, 4> ozoneAbsorptionHalfWidth{};
  std::array<float, 4> groundAlbedoRadianceScale{};
  std::array<float, 4> sunDirectionIntensity{};
  std::array<float, 4> sunColorAngularRadius{};
};
static_assert(sizeof(AtmosphereParametersGPU) == 128u);

AtmosphereParametersGPU makeAtmosphereParametersGPU(
    const AtmosphereProfile &profile, const SunSettings &sun);

bool finiteAtmosphereParametersGPU(const AtmosphereParametersGPU &parameters);

} // namespace atmosphere::rendering
