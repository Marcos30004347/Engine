#include "atmosphere/AtmosphereTypes.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace atmosphere
{
namespace
{

template <typename T, size_t Size>
bool arrayEqual(const std::array<T, Size> &left, const std::array<T, Size> &right)
{
  return left == right;
}

bool profileOpticalParametersEqual(
    const AtmosphereProfile &left, const AtmosphereProfile &right)
{
  return left.groundRadiusMetres == right.groundRadiusMetres &&
      left.topRadiusMetres == right.topRadiusMetres &&
      arrayEqual(left.rayleighScatteringPerMetre,
          right.rayleighScatteringPerMetre) &&
      left.rayleighScaleHeightMetres == right.rayleighScaleHeightMetres &&
      arrayEqual(left.mieScatteringPerMetre,
          right.mieScatteringPerMetre) &&
      arrayEqual(left.mieAbsorptionPerMetre, right.mieAbsorptionPerMetre) &&
      left.mieScaleHeightMetres == right.mieScaleHeightMetres &&
      left.mieAnisotropy == right.mieAnisotropy &&
      arrayEqual(left.ozoneAbsorptionPerMetre,
          right.ozoneAbsorptionPerMetre) &&
      left.ozoneCentreMetres == right.ozoneCentreMetres &&
      left.ozoneHalfWidthMetres == right.ozoneHalfWidthMetres;
}

bool finiteNonNegative(const Vec3f &value)
{
  for (const float component : value)
    if (!std::isfinite(component) || component < 0.0f)
      return false;
  return true;
}

bool fail(std::string *error, const char *message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

} // namespace

AtmosphereProfile earthAtmosphereProfile()
{
  // Hillaire 2020, Table 1, with coefficients converted from x10^-6 m^-1.
  return AtmosphereProfile{};
}

AtmosphereProfile vacuumAtmosphereProfile(const double groundRadiusMetres)
{
  AtmosphereProfile result{};
  result.groundRadiusMetres = std::max(groundRadiusMetres, 1.0);
  result.topRadiusMetres = result.groundRadiusMetres + 1.0;
  result.rayleighScatteringPerMetre = {0.0f, 0.0f, 0.0f};
  result.mieScatteringPerMetre = {0.0f, 0.0f, 0.0f};
  result.mieAbsorptionPerMetre = {0.0f, 0.0f, 0.0f};
  result.ozoneAbsorptionPerMetre = {0.0f, 0.0f, 0.0f};
  result.groundAlbedo = {0.0f, 0.0f, 0.0f};
  return result;
}

DayNightCycleSettings sanitizeDayNightCycleSettings(
    DayNightCycleSettings settings)
{
  const auto finiteOr = [](const double value, const double fallback)
  {
    return std::isfinite(value) ? value : fallback;
  };
  settings.timeOfDayHours = finiteOr(settings.timeOfDayHours, 12.0);
  settings.timeOfDayHours -=
      24.0 * std::floor(settings.timeOfDayHours / 24.0);
  settings.dayLengthSeconds =
      std::max(finiteOr(settings.dayLengthSeconds, 600.0), 0.001);
  settings.orbitalPeriodDays =
      std::max(finiteOr(settings.orbitalPeriodDays, 365.2422), 0.001);
  settings.dayOfYear = finiteOr(settings.dayOfYear, 0.0);
  settings.dayOfYear -= settings.orbitalPeriodDays *
      std::floor(settings.dayOfYear / settings.orbitalPeriodDays);
  settings.axialTiltDegrees = std::clamp(
      finiteOr(settings.axialTiltDegrees, 23.4393), -90.0, 90.0);
  settings.latitudeDegrees = std::clamp(
      finiteOr(settings.latitudeDegrees, 0.0), -90.0, 90.0);
  settings.longitudeDegrees = finiteOr(settings.longitudeDegrees, 0.0);
  settings.longitudeDegrees -= 360.0 *
      std::floor((settings.longitudeDegrees + 180.0) / 360.0);
  settings.timeScale = finiteOr(settings.timeScale, 1.0);
  settings.manualAzimuthDegrees =
      finiteOr(settings.manualAzimuthDegrees, 0.0);
  settings.manualAzimuthDegrees -= 360.0 *
      std::floor(settings.manualAzimuthDegrees / 360.0);
  settings.manualElevationDegrees = std::clamp(
      finiteOr(settings.manualElevationDegrees, 45.0), -90.0, 90.0);
  return settings;
}

void advanceDayNightCycle(
    DayNightCycleSettings &settings, const double realDeltaSeconds)
{
  settings = sanitizeDayNightCycleSettings(settings);
  if (!settings.enabled || settings.paused ||
      settings.mode != DayNightCycleMode::Celestial ||
      !std::isfinite(realDeltaSeconds))
    return;
  const double simulatedDays = realDeltaSeconds * settings.timeScale /
      settings.dayLengthSeconds;
  const double totalHours = settings.timeOfDayHours + simulatedDays * 24.0;
  const double wholeDays = std::floor(totalHours / 24.0);
  settings.timeOfDayHours = totalHours - wholeDays * 24.0;
  settings.dayOfYear += wholeDays;
  settings.dayOfYear -= settings.orbitalPeriodDays *
      std::floor(settings.dayOfYear / settings.orbitalPeriodDays);
}

DayNightSunState evaluateDayNightSun(
    const DayNightCycleSettings &settingsValue)
{
  constexpr double pi = 3.14159265358979323846;
  const DayNightCycleSettings settings =
      sanitizeDayNightCycleSettings(settingsValue);
  const double degreesToRadians = pi / 180.0;
  DayNightSunState result{};
  if (settings.mode == DayNightCycleMode::Manual)
  {
    const double azimuth = settings.manualAzimuthDegrees * degreesToRadians;
    const double elevation =
        settings.manualElevationDegrees * degreesToRadians;
    const double horizontal = std::cos(elevation);
    result.directionTowardSun = {horizontal * std::sin(azimuth),
        std::sin(elevation), horizontal * std::cos(azimuth)};
    result.elevationDegrees = settings.manualElevationDegrees;
  }
  else
  {
    const double latitude = settings.latitudeDegrees * degreesToRadians;
    const double axialTilt = settings.axialTiltDegrees * degreesToRadians;
    const double orbitalPhase = 2.0 * pi *
        (settings.dayOfYear / settings.orbitalPeriodDays);
    const double declination = std::asin(std::clamp(
        std::sin(axialTilt) * std::sin(orbitalPhase), -1.0, 1.0));
    const double hourAngle = 2.0 * pi *
        (settings.timeOfDayHours / 24.0 - 0.5) +
        settings.longitudeDegrees * degreesToRadians;
    const double cosDeclination = std::cos(declination);
    // Local frame is +X east, +Y up, +Z north.
    result.directionTowardSun = {
      -cosDeclination * std::sin(hourAngle),
      std::sin(latitude) * std::sin(declination) +
          std::cos(latitude) * cosDeclination * std::cos(hourAngle),
      std::cos(latitude) * std::sin(declination) -
          std::sin(latitude) * cosDeclination * std::cos(hourAngle),
    };
    result.elevationDegrees = std::asin(std::clamp(
        result.directionTowardSun[1], -1.0, 1.0)) / degreesToRadians;
    result.declinationDegrees = declination / degreesToRadians;
    result.hourAngleDegrees = hourAngle / degreesToRadians;
  }
  result.directionLightTravels = {-result.directionTowardSun[0],
      -result.directionTowardSun[1], -result.directionTowardSun[2]};
  return result;
}

bool dayNightCycleEqual(
    const DayNightCycleSettings &left,
    const DayNightCycleSettings &right)
{
  return left.enabled == right.enabled && left.paused == right.paused &&
      left.mode == right.mode &&
      left.timeOfDayHours == right.timeOfDayHours &&
      left.dayLengthSeconds == right.dayLengthSeconds &&
      left.dayOfYear == right.dayOfYear &&
      left.orbitalPeriodDays == right.orbitalPeriodDays &&
      left.axialTiltDegrees == right.axialTiltDegrees &&
      left.latitudeDegrees == right.latitudeDegrees &&
      left.longitudeDegrees == right.longitudeDegrees &&
      left.timeScale == right.timeScale &&
      left.manualAzimuthDegrees == right.manualAzimuthDegrees &&
      left.manualElevationDegrees == right.manualElevationDegrees;
}

AtmosphereViewMode updateAtmosphereViewMode(
    const AtmosphereViewMode current,
    const double altitudeMetres,
    const double atmosphereHeightMetres,
    const double orbitEnterFraction,
    const double orbitLeaveFraction)
{
  const double altitude = std::isfinite(altitudeMetres)
      ? std::max(altitudeMetres, 0.0) : 0.0;
  const double height = std::isfinite(atmosphereHeightMetres)
      ? std::max(atmosphereHeightMetres, 0.0) : 0.0;
  const double enter = std::clamp(
      std::isfinite(orbitEnterFraction) ? orbitEnterFraction : 0.20,
      0.0, 1.0);
  const double leave = std::clamp(
      std::isfinite(orbitLeaveFraction) ? orbitLeaveFraction : 0.15,
      0.0, enter);
  if (!(height > 0.0))
    return AtmosphereViewMode::Ground;
  if (current == AtmosphereViewMode::Ground && altitude > height * enter)
    return AtmosphereViewMode::Orbit;
  if (current == AtmosphereViewMode::Orbit && altitude < height * leave)
    return AtmosphereViewMode::Ground;
  return current;
}

AtmosphereScreenBounds projectedAtmosphereScreenBounds(
    const uint32_t viewportWidth,
    const uint32_t viewportHeight,
    const double verticalFovRadians,
    const double cameraDistanceRadii,
    const double topRadiusRadii,
    const uint32_t paddingPixels)
{
  constexpr double pi = 3.14159265358979323846;
  if (viewportWidth == 0u || viewportHeight == 0u ||
      !std::isfinite(verticalFovRadians) || verticalFovRadians <= 0.0 ||
      verticalFovRadians >= pi || !std::isfinite(cameraDistanceRadii) ||
      !std::isfinite(topRadiusRadii) || cameraDistanceRadii <= 0.0 ||
      topRadiusRadii <= 0.0)
    return {};
  if (cameraDistanceRadii <= topRadiusRadii)
    return {0u, 0u, viewportWidth, viewportHeight};
  const double angularRadius = std::asin(std::clamp(
      topRadiusRadii / cameraDistanceRadii, 0.0, 1.0));
  const double focalPixels = 0.5 * static_cast<double>(viewportHeight) /
      std::tan(verticalFovRadians * 0.5);
  const double radiusPixels = std::tan(angularRadius) * focalPixels +
      static_cast<double>(paddingPixels);
  if (!std::isfinite(radiusPixels) || radiusPixels <= 0.0)
    return {};
  const int64_t centreX = static_cast<int64_t>(viewportWidth) / 2;
  const int64_t centreY = static_cast<int64_t>(viewportHeight) / 2;
  const int64_t radius = static_cast<int64_t>(std::ceil(radiusPixels));
  const int64_t minimumX = std::clamp<int64_t>(centreX - radius, 0,
      static_cast<int64_t>(viewportWidth));
  const int64_t minimumY = std::clamp<int64_t>(centreY - radius, 0,
      static_cast<int64_t>(viewportHeight));
  const int64_t maximumX = std::clamp<int64_t>(centreX + radius + 1, 0,
      static_cast<int64_t>(viewportWidth));
  const int64_t maximumY = std::clamp<int64_t>(centreY + radius + 1, 0,
      static_cast<int64_t>(viewportHeight));
  return {static_cast<uint32_t>(minimumX),
      static_cast<uint32_t>(minimumY),
      static_cast<uint32_t>(std::max<int64_t>(maximumX - minimumX, 0)),
      static_cast<uint32_t>(std::max<int64_t>(maximumY - minimumY, 0))};
}

bool validateAtmosphereProfile(
    const AtmosphereProfile &profile, std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (!std::isfinite(profile.groundRadiusMetres) ||
      !std::isfinite(profile.topRadiusMetres) ||
      profile.groundRadiusMetres <= 0.0 ||
      profile.topRadiusMetres <= profile.groundRadiusMetres)
    return fail(error, "Atmosphere radii must be finite and top radius must exceed ground radius");
  if (!finiteNonNegative(profile.rayleighScatteringPerMetre) ||
      !finiteNonNegative(profile.mieScatteringPerMetre) ||
      !finiteNonNegative(profile.mieAbsorptionPerMetre) ||
      !finiteNonNegative(profile.ozoneAbsorptionPerMetre))
    return fail(error, "Atmosphere coefficients must be finite and non-negative");
  if (!std::isfinite(profile.rayleighScaleHeightMetres) ||
      !std::isfinite(profile.mieScaleHeightMetres) ||
      profile.rayleighScaleHeightMetres <= 0.0f ||
      profile.mieScaleHeightMetres <= 0.0f)
    return fail(error, "Atmosphere scale heights must be finite and positive");
  if (!std::isfinite(profile.mieAnisotropy) ||
      profile.mieAnisotropy <= -1.0f || profile.mieAnisotropy >= 1.0f)
    return fail(error, "Mie anisotropy must be finite and strictly between -1 and 1");
  if (!std::isfinite(profile.ozoneCentreMetres) ||
      !std::isfinite(profile.ozoneHalfWidthMetres) ||
      profile.ozoneCentreMetres < 0.0f ||
      profile.ozoneHalfWidthMetres <= 0.0f)
    return fail(error, "Ozone distribution must have a non-negative centre and positive half width");
  if (!finiteNonNegative(profile.groundAlbedo) ||
      std::any_of(profile.groundAlbedo.begin(), profile.groundAlbedo.end(),
          [](const float value) { return value > 1.0f; }))
    return fail(error, "Ground albedo must be finite and in [0, 1]");
  if (!std::isfinite(profile.atmosphereRadianceScale) ||
      profile.atmosphereRadianceScale < 0.0f)
    return fail(error, "Atmosphere radiance scale must be finite and non-negative");
  return true;
}

AtmosphereDirtyFlags classifyAtmosphereChanges(
    const AtmosphereState &previous, const AtmosphereState &current)
{
  AtmosphereDirtyFlags result = AtmosphereDirtyFlags::None;
  const bool opticalChanged =
      !profileOpticalParametersEqual(previous.profile, current.profile);
  if (opticalChanged)
    result |= AtmosphereDirtyFlags::All;
  else
  {
    if (!arrayEqual(previous.profile.groundAlbedo,
            current.profile.groundAlbedo))
      result |= AtmosphereDirtyFlags::MultipleScattering |
          AtmosphereDirtyFlags::SkyView |
          AtmosphereDirtyFlags::AerialPerspective |
          AtmosphereDirtyFlags::Ambient;
    if (previous.profile.atmosphereRadianceScale !=
        current.profile.atmosphereRadianceScale)
      result |= AtmosphereDirtyFlags::MultipleScattering |
          AtmosphereDirtyFlags::SkyView |
          AtmosphereDirtyFlags::AerialPerspective |
          AtmosphereDirtyFlags::Ambient;
  }

  if (previous.sun.enabled != current.sun.enabled ||
      !arrayEqual(previous.sun.directionPlanetSpace,
          current.sun.directionPlanetSpace) ||
      !arrayEqual(previous.sun.color, current.sun.color) ||
      previous.sun.intensity != current.sun.intensity ||
      previous.sun.angularRadiusRadians != current.sun.angularRadiusRadians ||
      previous.sun.drawDisk != current.sun.drawDisk ||
      previous.sun.diskLuminanceScale != current.sun.diskLuminanceScale)
    result |= AtmosphereDirtyFlags::SkyView |
        AtmosphereDirtyFlags::AerialPerspective |
        AtmosphereDirtyFlags::Ambient;

  if (!arrayEqual(previous.view.cameraPlanetPositionMetres,
          current.view.cameraPlanetPositionMetres) ||
      !arrayEqual(previous.view.localUpPlanetSpace,
          current.view.localUpPlanetSpace))
    result |= AtmosphereDirtyFlags::SkyView |
        AtmosphereDirtyFlags::AerialPerspective |
        AtmosphereDirtyFlags::Ambient;

  if (previous.view.verticalFovRadians != current.view.verticalFovRadians ||
      previous.view.viewportWidth != current.view.viewportWidth ||
      previous.view.viewportHeight != current.view.viewportHeight)
    result |= AtmosphereDirtyFlags::AerialPerspective;
  return result;
}

void advanceAtmosphereRevisions(
    AtmosphereRevisions &revisions,
    const AtmosphereDirtyFlags dirty,
    const bool discontinuity)
{
  if (hasFlag(dirty, AtmosphereDirtyFlags::Transmittance))
    ++revisions.transmittance;
  if (hasFlag(dirty, AtmosphereDirtyFlags::MultipleScattering))
    ++revisions.multipleScattering;
  if (hasFlag(dirty, AtmosphereDirtyFlags::SkyView))
    ++revisions.skyView;
  if (hasFlag(dirty, AtmosphereDirtyFlags::AerialPerspective))
    ++revisions.aerialPerspective;
  if (hasFlag(dirty, AtmosphereDirtyFlags::Ambient))
    ++revisions.ambient;
  if (hasFlag(dirty, AtmosphereDirtyFlags::Transmittance) ||
      hasFlag(dirty, AtmosphereDirtyFlags::MultipleScattering))
    ++revisions.profile;
  if (discontinuity)
    ++revisions.discontinuityEpoch;
}

QuantizedSunRevisionKey quantizedSunRevisionKey(const SunSettings &sun,
    const double angularStepDegrees,
    const double relativeRadianceStep)
{
  constexpr double pi = 3.14159265358979323846;
  const double angularStep = std::max(angularStepDegrees, 1.0e-6) *
      (pi / 180.0);
  const double length = std::sqrt(
      sun.directionPlanetSpace[0] * sun.directionPlanetSpace[0] +
      sun.directionPlanetSpace[1] * sun.directionPlanetSpace[1] +
      sun.directionPlanetSpace[2] * sun.directionPlanetSpace[2]);
  Vec3d direction{0.0, -1.0, 0.0};
  if (std::isfinite(length) && length > 1.0e-12)
    for (size_t component = 0u; component < direction.size(); ++component)
      direction[component] = sun.directionPlanetSpace[component] / length;
  QuantizedSunRevisionKey result{};
  result.enabled = sun.enabled;
  result.azimuthBin = static_cast<int32_t>(std::llround(
      std::atan2(direction[2], direction[0]) / angularStep));
  result.elevationBin = static_cast<int32_t>(std::llround(
      std::asin(std::clamp(direction[1], -1.0, 1.0)) / angularStep));
  const double logarithmicStep = std::log1p(
      std::clamp(relativeRadianceStep, 1.0e-6, 1.0));
  for (size_t channel = 0u; channel < result.radianceBins.size(); ++channel)
  {
    const double radiance = sun.enabled
        ? std::max(static_cast<double>(sun.color[channel]) *
              static_cast<double>(sun.intensity), 0.0)
        : 0.0;
    result.radianceBins[channel] = radiance > 0.0
        ? static_cast<int32_t>(std::llround(std::log(radiance) /
              logarithmicStep))
        : std::numeric_limits<int32_t>::min();
  }
  return result;
}

float terrainShadowCoverageWeight(const bool coverageAvailable,
    const double sunZenithCosine, const double normalizedMapRadius,
    const double minimumSunZenithCosine,
    const double fullSunZenithCosine, const double edgeFadeStart)
{
  if (!coverageAvailable || !std::isfinite(sunZenithCosine) ||
      !std::isfinite(normalizedMapRadius))
    return 0.0f;
  const double minimumSun = std::clamp(
      std::isfinite(minimumSunZenithCosine)
          ? minimumSunZenithCosine : 0.04,
      0.0, 1.0);
  const double fullSun = std::clamp(
      std::isfinite(fullSunZenithCosine) ? fullSunZenithCosine : 0.12,
      minimumSun + 1.0e-6, 1.0);
  const double edgeStart = std::clamp(
      std::isfinite(edgeFadeStart) ? edgeFadeStart : 0.82, 0.0, 0.999999);
  const auto smoothstep = [](const double low, const double high,
                             const double value)
  {
    const double t = std::clamp((value - low) / (high - low), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
  };
  const double sunWeight = smoothstep(minimumSun, fullSun,
      std::clamp(sunZenithCosine, -1.0, 1.0));
  const double radius = std::max(normalizedMapRadius, 0.0);
  const double edgeWeight = 1.0 - smoothstep(edgeStart, 1.0, radius);
  return static_cast<float>(std::clamp(sunWeight * edgeWeight, 0.0, 1.0));
}

bool terrainShadowMapNeedsUpdate(
    const QuantizedSunRevisionKey &previous,
    const QuantizedSunRevisionKey &current, const bool hasPrevious,
    const bool anchorChanged, const bool coverageAvailable)
{
  return coverageAvailable &&
      (!hasPrevious || anchorChanged || !(previous == current));
}

} // namespace atmosphere
