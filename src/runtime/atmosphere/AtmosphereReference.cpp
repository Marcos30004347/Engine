#include "atmosphere/AtmosphereReference.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>

namespace atmosphere
{
namespace
{

double dot(const Vec3d &left, const Vec3d &right)
{
  return left[0] * right[0] + left[1] * right[1] +
      left[2] * right[2];
}

Vec3d addScaled(
    const Vec3d &origin, const Vec3d &direction, const double distance)
{
  return {origin[0] + direction[0] * distance,
          origin[1] + direction[1] * distance,
          origin[2] + direction[2] * distance};
}

double length(const Vec3d &value)
{
  return std::sqrt(std::max(dot(value, value), 0.0));
}

Vec3d normalized(const Vec3d &value)
{
  const double magnitude = length(value);
  if (!(magnitude > 1.0e-15))
    return {0.0, 1.0, 0.0};
  return {value[0] / magnitude, value[1] / magnitude,
          value[2] / magnitude};
}

double clamp01(const double value)
{
  return std::clamp(value, 0.0, 1.0);
}

bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

uint8_t displayByte(const double transmittance)
{
  const double encoded = std::pow(clamp01(transmittance), 1.0 / 2.2);
  return static_cast<uint8_t>(std::lround(encoded * 255.0));
}

} // namespace

RaySphereInterval intersectRaySphere(
    const Vec3d &origin, const Vec3d &directionValue, const double radius)
{
  const double a = dot(directionValue, directionValue);
  if (!(a > 1.0e-30) || !(radius > 0.0))
    return {};
  const double b = 2.0 * dot(origin, directionValue);
  const double c = dot(origin, origin) - radius * radius;
  const double discriminant = b * b - 4.0 * a * c;
  if (discriminant < 0.0)
    return {};
  const double root = std::sqrt(std::max(discriminant, 0.0));
  double nearDistance = (-b - root) / (2.0 * a);
  double farDistance = (-b + root) / (2.0 * a);
  if (nearDistance > farDistance)
    std::swap(nearDistance, farDistance);
  return {true, nearDistance, farDistance};
}

AtmosphereDensitySample atmosphereDensity(
    const AtmosphereProfile &profile, const double altitudeMetres)
{
  const double altitude = std::max(altitudeMetres, 0.0);
  const double atmosphereHeight =
      profile.topRadiusMetres - profile.groundRadiusMetres;
  if (altitude > atmosphereHeight)
    return {};
  const double rayleigh = std::exp(
      -altitude / std::max<double>(profile.rayleighScaleHeightMetres, 1.0));
  const double mie = std::exp(
      -altitude / std::max<double>(profile.mieScaleHeightMetres, 1.0));
  const double ozone = std::max(
      0.0, 1.0 - std::abs(altitude - profile.ozoneCentreMetres) /
          std::max<double>(profile.ozoneHalfWidthMetres, 1.0));
  return {rayleigh, mie, ozone};
}

Vec3d atmosphereExtinctionPerMetre(
    const AtmosphereProfile &profile, const double altitudeMetres)
{
  const AtmosphereDensitySample density =
      atmosphereDensity(profile, altitudeMetres);
  Vec3d result{};
  for (size_t channel = 0u; channel < result.size(); ++channel)
    result[channel] =
        static_cast<double>(profile.rayleighScatteringPerMetre[channel]) *
            density.rayleigh +
        static_cast<double>(profile.mieScatteringPerMetre[channel] +
            profile.mieAbsorptionPerMetre[channel]) * density.mie +
        static_cast<double>(profile.ozoneAbsorptionPerMetre[channel]) *
            density.ozone;
  return result;
}

Vec3d transmittanceAlongSegment(
    const AtmosphereProfile &profile,
    const Vec3d &originMetres,
    const Vec3d &directionValue,
    const double distanceMetres,
    const uint32_t integrationSteps)
{
  if (!(distanceMetres > 0.0))
    return {1.0, 1.0, 1.0};
  const Vec3d direction = normalized(directionValue);
  const uint32_t steps = std::max(integrationSteps, 1u);
  const double stepLength = distanceMetres / static_cast<double>(steps);
  Vec3d opticalDepth{};
  for (uint32_t step = 0u; step < steps; ++step)
  {
    const double distance = (static_cast<double>(step) + 0.5) * stepLength;
    const Vec3d position = addScaled(originMetres, direction, distance);
    const double altitude = length(position) - profile.groundRadiusMetres;
    const Vec3d extinction = atmosphereExtinctionPerMetre(profile, altitude);
    for (size_t channel = 0u; channel < opticalDepth.size(); ++channel)
      opticalDepth[channel] += extinction[channel] * stepLength;
  }
  return {std::exp(-opticalDepth[0]), std::exp(-opticalDepth[1]),
          std::exp(-opticalDepth[2])};
}

std::array<double, 2> transmittanceLutUv(
    const AtmosphereProfile &profile,
    const TransmittanceLutParameters &parameters)
{
  const double ground = profile.groundRadiusMetres;
  const double top = profile.topRadiusMetres;
  const double height = std::sqrt(std::max(top * top - ground * ground, 0.0));
  const double radius = std::clamp(parameters.radiusMetres, ground, top);
  const double rho = std::sqrt(std::max(radius * radius - ground * ground, 0.0));
  const double distance = distanceToAtmosphereTop(
      profile, radius, parameters.viewZenithCosine);
  const double minimumDistance = top - radius;
  const double maximumDistance = rho + height;
  const double denominator = maximumDistance - minimumDistance;
  const double u = denominator > 1.0e-12
      ? (distance - minimumDistance) / denominator
      : 0.0;
  return {clamp01(u), height > 0.0 ? clamp01(rho / height) : 0.0};
}

TransmittanceLutParameters transmittanceLutParameters(
    const AtmosphereProfile &profile,
    const std::array<double, 2> &uvValue)
{
  const double u = clamp01(uvValue[0]);
  const double v = clamp01(uvValue[1]);
  const double ground = profile.groundRadiusMetres;
  const double top = profile.topRadiusMetres;
  const double height = std::sqrt(std::max(top * top - ground * ground, 0.0));
  const double rho = height * v;
  const double radius = std::sqrt(rho * rho + ground * ground);
  const double minimumDistance = top - radius;
  const double maximumDistance = rho + height;
  const double distance = minimumDistance + u *
      (maximumDistance - minimumDistance);
  double cosine = 1.0;
  if (distance > 1.0e-12 && radius > 0.0)
    cosine = (height * height - rho * rho - distance * distance) /
        (2.0 * radius * distance);
  return {radius, std::clamp(cosine, -1.0, 1.0)};
}

Vec3d skyViewDirection(const std::array<double, 2> &uvValue)
{
  constexpr double pi = 3.14159265358979323846;
  const double u = clamp01(uvValue[0]);
  const double encodedLatitude = std::clamp(uvValue[1] * 2.0 - 1.0,
      -1.0, 1.0);
  const double latitudeNormalized = std::copysign(
      encodedLatitude * encodedLatitude, encodedLatitude);
  const double elevation = latitudeNormalized * 0.5 * pi;
  const double azimuth = (u - 0.5) * 2.0 * pi;
  const double horizontal = std::cos(elevation);
  return {horizontal * std::cos(azimuth), std::sin(elevation),
          horizontal * std::sin(azimuth)};
}

std::array<double, 2> skyViewUv(const Vec3d &directionValue)
{
  constexpr double pi = 3.14159265358979323846;
  const Vec3d direction = normalized(directionValue);
  const double latitudeNormalized = std::asin(
      std::clamp(direction[1], -1.0, 1.0)) / (0.5 * pi);
  const double encodedLatitude = std::copysign(
      std::sqrt(std::abs(latitudeNormalized)), latitudeNormalized);
  const double azimuth = std::atan2(direction[2], direction[0]);
  double u = azimuth / (2.0 * pi) + 0.5;
  u -= std::floor(u);
  return {u, clamp01(encodedLatitude * 0.5 + 0.5)};
}

double aerialPerspectiveDistanceMetres(const double normalizedDepth,
    const double maximumDistanceMetres,
    const double distributionScaleMetres)
{
  const double maximumDistance = std::max(maximumDistanceMetres, 0.0);
  const double scale = std::max(distributionScaleMetres, 1.0e-6);
  return scale * std::expm1(clamp01(normalizedDepth) *
      std::log1p(maximumDistance / scale));
}

double aerialPerspectiveNormalizedDepth(const double distanceMetres,
    const double maximumDistanceMetres,
    const double distributionScaleMetres)
{
  const double maximumDistance = std::max(maximumDistanceMetres, 0.0);
  if (!(maximumDistance > 0.0))
    return 0.0;
  const double scale = std::max(distributionScaleMetres, 1.0e-6);
  return clamp01(std::log1p(std::clamp(distanceMetres, 0.0,
      maximumDistance) / scale) /
      std::log1p(maximumDistance / scale));
}

AtmosphereAmbientSH1 projectAtmosphereAmbientSH1(
    const std::vector<Vec3d> &directions,
    const std::vector<Vec3d> &radiances)
{
  constexpr double y00 = 0.28209479177387814;
  constexpr double y1 = 0.48860251190291992;
  AtmosphereAmbientSH1 result{};
  const size_t count = std::min(directions.size(), radiances.size());
  if (count == 0u)
    return result;
  const double weight = 4.0 * 3.14159265358979323846 /
      static_cast<double>(count);
  for (size_t sample = 0u; sample < count; ++sample)
  {
    const Vec3d direction = normalized(directions[sample]);
    const std::array<double, 4> basis{
      y00, y1 * direction[0], y1 * direction[1], y1 * direction[2]};
    for (size_t coefficient = 0u; coefficient < basis.size(); ++coefficient)
      for (size_t channel = 0u; channel < 3u; ++channel)
        result.coefficients[coefficient][channel] +=
            std::max(radiances[sample][channel], 0.0) *
            basis[coefficient] * weight;
  }
  return result;
}

Vec3d evaluateAtmosphereAmbientSH1(
    const AtmosphereAmbientSH1 &sh, const Vec3d &directionValue)
{
  constexpr double y00 = 0.28209479177387814;
  constexpr double y1 = 0.48860251190291992;
  const Vec3d direction = normalized(directionValue);
  const std::array<double, 4> basis{
    y00, y1 * direction[0], y1 * direction[1], y1 * direction[2]};
  Vec3d result{};
  for (size_t coefficient = 0u; coefficient < basis.size(); ++coefficient)
    for (size_t channel = 0u; channel < result.size(); ++channel)
      result[channel] += sh.coefficients[coefficient][channel] *
          basis[coefficient];
  for (double &channel : result) channel = std::max(channel, 0.0);
  return result;
}

Vec3d meanAtmosphereAmbientRadiance(const AtmosphereAmbientSH1 &sh)
{
  constexpr double y00 = 0.28209479177387814;
  return {std::max(sh.coefficients[0][0] * y00, 0.0),
          std::max(sh.coefficients[0][1] * y00, 0.0),
          std::max(sh.coefficients[0][2] * y00, 0.0)};
}

double distanceToAtmosphereTop(
    const AtmosphereProfile &profile,
    const double radiusMetres,
    const double viewZenithCosineValue)
{
  const double radius = std::clamp(
      radiusMetres, profile.groundRadiusMetres, profile.topRadiusMetres);
  const double cosine = std::clamp(viewZenithCosineValue, -1.0, 1.0);
  const double discriminant = radius * radius *
      (cosine * cosine - 1.0) +
      profile.topRadiusMetres * profile.topRadiusMetres;
  return std::max(-radius * cosine +
      std::sqrt(std::max(discriminant, 0.0)), 0.0);
}

Vec3d referenceTransmittanceToAtmosphereTop(
    const AtmosphereProfile &profile,
    const double radiusMetres,
    const double viewZenithCosine,
    const uint32_t integrationSteps)
{
  const double cosine = std::clamp(viewZenithCosine, -1.0, 1.0);
  const Vec3d origin{0.0, radiusMetres, 0.0};
  const Vec3d direction{std::sqrt(std::max(1.0 - cosine * cosine, 0.0)),
                        cosine, 0.0};
  return transmittanceAlongSegment(
      profile, origin, direction,
      distanceToAtmosphereTop(profile, radiusMetres, cosine),
      integrationSteps);
}

bool writeTransmittanceReference(
    const std::filesystem::path &outputDirectory,
    const AtmosphereProfile &profile,
    const AtmosphereReferenceExportSettings &settings,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  std::string validationError;
  if (!validateAtmosphereProfile(profile, &validationError))
    return fail(error, validationError);
  if (settings.width == 0u || settings.height == 0u ||
      settings.integrationSteps == 0u)
    return fail(error, "Reference dimensions and integration step count must be non-zero");

  std::error_code filesystemError;
  std::filesystem::create_directories(outputDirectory, filesystemError);
  if (filesystemError)
    return fail(error, "Failed to create output directory: " +
        filesystemError.message());

  std::ofstream csv(outputDirectory / "transmittance.csv",
      std::ios::trunc);
  std::ofstream probes(outputDirectory / "transmittance-probes.csv",
      std::ios::trunc);
  std::ofstream ppm(outputDirectory / "transmittance.ppm",
      std::ios::binary | std::ios::trunc);
  if (!csv || !probes || !ppm)
    return fail(error, "Failed to open transmittance reference outputs");

  csv << std::setprecision(17);
  csv << "x,y,u,v,altitude_metres,view_zenith_cosine,distance_metres,transmittance_r,transmittance_g,transmittance_b\n";
  probes << std::setprecision(17);
  probes << "altitude_metres,view,view_zenith_cosine,distance_metres,transmittance_r,transmittance_g,transmittance_b\n";
  ppm << "P6\n" << settings.width << ' ' << settings.height << "\n255\n";

  for (uint32_t y = 0u; y < settings.height; ++y)
  {
    for (uint32_t x = 0u; x < settings.width; ++x)
    {
      const std::array<double, 2> uv{
          (static_cast<double>(x) + 0.5) / settings.width,
          (static_cast<double>(y) + 0.5) / settings.height};
      const TransmittanceLutParameters parameters =
          transmittanceLutParameters(profile, uv);
      const double distance = distanceToAtmosphereTop(
          profile, parameters.radiusMetres,
          parameters.viewZenithCosine);
      const Vec3d transmittance = referenceTransmittanceToAtmosphereTop(
          profile, parameters.radiusMetres,
          parameters.viewZenithCosine, settings.integrationSteps);
      csv << x << ',' << y << ',' << uv[0] << ',' << uv[1] << ','
          << parameters.radiusMetres - profile.groundRadiusMetres << ','
          << parameters.viewZenithCosine << ',' << distance << ','
          << transmittance[0] << ',' << transmittance[1] << ','
          << transmittance[2] << '\n';
      const std::array<uint8_t, 3> pixel{
          displayByte(transmittance[0]), displayByte(transmittance[1]),
          displayByte(transmittance[2])};
      ppm.write(reinterpret_cast<const char *>(pixel.data()), pixel.size());
    }
  }

  constexpr std::array<double, 5> probeAltitudesMetres{
      0.0, 2'000.0, 10'000.0, 50'000.0, 90'000.0};
  constexpr std::array<double, 2> probeCosines{1.0, 0.0};
  constexpr std::array<const char *, 2> probeNames{"zenith", "horizon"};
  const double atmosphereHeight =
      profile.topRadiusMetres - profile.groundRadiusMetres;
  for (const double requestedAltitude : probeAltitudesMetres)
  {
    const double altitude = std::min(requestedAltitude, atmosphereHeight);
    const double radius = profile.groundRadiusMetres + altitude;
    for (size_t view = 0u; view < probeCosines.size(); ++view)
    {
      const double cosine = probeCosines[view];
      const double distance = distanceToAtmosphereTop(profile, radius, cosine);
      const Vec3d transmittance = referenceTransmittanceToAtmosphereTop(
          profile, radius, cosine, settings.integrationSteps);
      probes << altitude << ',' << probeNames[view] << ',' << cosine << ','
             << distance << ',' << transmittance[0] << ','
             << transmittance[1] << ',' << transmittance[2] << '\n';
    }
  }
  if (!csv || !probes || !ppm)
    return fail(error, "Failed while writing transmittance reference outputs");

  std::ofstream manifest(outputDirectory / "reference.txt", std::ios::trunc);
  if (!manifest)
    return fail(error, "Failed to open transmittance reference manifest");
  manifest << "model=Hillaire2020\n"
           << "length_unit=" << kAtmosphereLengthUnit << '\n'
           << "light_direction_convention="
           << kAtmosphereLightDirectionConvention << '\n'
           << "width=" << settings.width << '\n'
           << "height=" << settings.height << '\n'
           << "integration_steps=" << settings.integrationSteps << '\n'
           << "probe_altitudes_metres=0,2000,10000,50000,90000\n"
           << "probe_views=zenith,horizon\n"
           << "ground_radius_metres=" << profile.groundRadiusMetres << '\n'
           << "top_radius_metres=" << profile.topRadiusMetres << '\n';
  return static_cast<bool>(manifest);
}

} // namespace atmosphere
