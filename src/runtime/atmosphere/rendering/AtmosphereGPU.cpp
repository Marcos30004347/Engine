#include "atmosphere/rendering/AtmosphereGPU.hpp"

#include <algorithm>
#include <cmath>

namespace atmosphere::rendering
{
namespace
{

std::array<double, 3> normalizedDirection(const Vec3d &value)
{
  const double lengthSquared = value[0] * value[0] + value[1] * value[1] +
      value[2] * value[2];
  if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-20)
    return {0.0, -1.0, 0.0};
  const double inverseLength = 1.0 / std::sqrt(lengthSquared);
  return {value[0] * inverseLength, value[1] * inverseLength,
          value[2] * inverseLength};
}

} // namespace

AtmosphereParametersGPU makeAtmosphereParametersGPU(
    const AtmosphereProfile &profile, const SunSettings &sun)
{
  constexpr float metresToKilometres = 0.001f;
  constexpr float perMetreToPerKilometre = 1000.0f;
  const auto direction = normalizedDirection(sun.directionPlanetSpace);
  AtmosphereParametersGPU result{};
  result.radiiMieG = {
    static_cast<float>(profile.groundRadiusMetres) * metresToKilometres,
    static_cast<float>(profile.topRadiusMetres) * metresToKilometres,
    static_cast<float>(profile.topRadiusMetres - profile.groundRadiusMetres) *
        metresToKilometres,
    profile.mieAnisotropy,
  };
  result.rayleighScatteringScale = {
    profile.rayleighScatteringPerMetre[0] * perMetreToPerKilometre,
    profile.rayleighScatteringPerMetre[1] * perMetreToPerKilometre,
    profile.rayleighScatteringPerMetre[2] * perMetreToPerKilometre,
    profile.rayleighScaleHeightMetres * metresToKilometres,
  };
  result.mieScatteringScale = {
    profile.mieScatteringPerMetre[0] * perMetreToPerKilometre,
    profile.mieScatteringPerMetre[1] * perMetreToPerKilometre,
    profile.mieScatteringPerMetre[2] * perMetreToPerKilometre,
    profile.mieScaleHeightMetres * metresToKilometres,
  };
  result.mieAbsorptionOzoneCentre = {
    profile.mieAbsorptionPerMetre[0] * perMetreToPerKilometre,
    profile.mieAbsorptionPerMetre[1] * perMetreToPerKilometre,
    profile.mieAbsorptionPerMetre[2] * perMetreToPerKilometre,
    profile.ozoneCentreMetres * metresToKilometres,
  };
  result.ozoneAbsorptionHalfWidth = {
    profile.ozoneAbsorptionPerMetre[0] * perMetreToPerKilometre,
    profile.ozoneAbsorptionPerMetre[1] * perMetreToPerKilometre,
    profile.ozoneAbsorptionPerMetre[2] * perMetreToPerKilometre,
    profile.ozoneHalfWidthMetres * metresToKilometres,
  };
  result.groundAlbedoRadianceScale = {
    profile.groundAlbedo[0], profile.groundAlbedo[1],
    profile.groundAlbedo[2], profile.atmosphereRadianceScale,
  };
  result.sunDirectionIntensity = {
    static_cast<float>(direction[0]), static_cast<float>(direction[1]),
    static_cast<float>(direction[2]),
    sun.enabled ? std::max(sun.intensity, 0.0f) : 0.0f,
  };
  result.sunColorAngularRadius = {
    std::max(sun.color[0], 0.0f), std::max(sun.color[1], 0.0f),
    std::max(sun.color[2], 0.0f),
    std::max(sun.angularRadiusRadians, 0.0f),
  };
  return result;
}

bool finiteAtmosphereParametersGPU(
    const AtmosphereParametersGPU &parameters)
{
  const std::array<std::array<float, 4>, 8> values{
    parameters.radiiMieG,
    parameters.rayleighScatteringScale,
    parameters.mieScatteringScale,
    parameters.mieAbsorptionOzoneCentre,
    parameters.ozoneAbsorptionHalfWidth,
    parameters.groundAlbedoRadianceScale,
    parameters.sunDirectionIntensity,
    parameters.sunColorAngularRadius,
  };
  for (const auto &value : values)
    for (const float component : value)
      if (!std::isfinite(component))
        return false;
  return true;
}

} // namespace atmosphere::rendering
