#pragma once

#include "atmosphere/AtmosphereTypes.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace atmosphere
{

struct RaySphereInterval
{
  bool hit = false;
  double nearDistance = 0.0;
  double farDistance = 0.0;
};

struct AtmosphereDensitySample
{
  double rayleigh = 0.0;
  double mie = 0.0;
  double ozone = 0.0;
};

struct TransmittanceLutParameters
{
  double radiusMetres = 0.0;
  double viewZenithCosine = 1.0;
};

struct AtmosphereReferenceExportSettings
{
  uint32_t width = 256u;
  uint32_t height = 64u;
  uint32_t integrationSteps = 256u;
};

struct AtmosphereAmbientSH1
{
  // L0, L1-X, L1-Y, L1-Z RGB coefficients using the real orthonormal basis.
  std::array<Vec3d, 4> coefficients{};
};

RaySphereInterval intersectRaySphere(
    const Vec3d &origin, const Vec3d &direction, double radius);

AtmosphereDensitySample atmosphereDensity(
    const AtmosphereProfile &profile, double altitudeMetres);

Vec3d atmosphereExtinctionPerMetre(
    const AtmosphereProfile &profile, double altitudeMetres);

Vec3d transmittanceAlongSegment(
    const AtmosphereProfile &profile,
    const Vec3d &originMetres,
    const Vec3d &direction,
    double distanceMetres,
    uint32_t integrationSteps);

// Maps radius/view cosine to the normalized transmittance-LUT domain used by
// Bruneton and Hillaire. Only rays reaching the top atmosphere boundary are
// represented; callers handle ground occlusion separately.
std::array<double, 2> transmittanceLutUv(
    const AtmosphereProfile &profile,
    const TransmittanceLutParameters &parameters);

TransmittanceLutParameters transmittanceLutParameters(
    const AtmosphereProfile &profile,
    const std::array<double, 2> &uv);

// Canonical Sky-View mapping used by the GPU LUT: +Y is local up, and the
// projected direction toward the sun is +X. Longitude wraps in U; V uses a
// signed square-root warp around the horizon.
Vec3d skyViewDirection(const std::array<double, 2> &uv);
std::array<double, 2> skyViewUv(const Vec3d &direction);

// Logarithmic aerial-perspective depth mapping. The distribution scale sets
// the distance over which slices remain approximately linear; beyond it the
// spacing grows logarithmically until maximumDistanceMetres.
double aerialPerspectiveDistanceMetres(double normalizedDepth,
    double maximumDistanceMetres, double distributionScaleMetres = 100.0);
double aerialPerspectiveNormalizedDepth(double distanceMetres,
    double maximumDistanceMetres, double distributionScaleMetres = 100.0);

AtmosphereAmbientSH1 projectAtmosphereAmbientSH1(
    const std::vector<Vec3d> &directions,
    const std::vector<Vec3d> &radiances);
Vec3d evaluateAtmosphereAmbientSH1(
    const AtmosphereAmbientSH1 &sh, const Vec3d &direction);
Vec3d meanAtmosphereAmbientRadiance(const AtmosphereAmbientSH1 &sh);

double distanceToAtmosphereTop(
    const AtmosphereProfile &profile,
    double radiusMetres,
    double viewZenithCosine);

Vec3d referenceTransmittanceToAtmosphereTop(
    const AtmosphereProfile &profile,
    double radiusMetres,
    double viewZenithCosine,
    uint32_t integrationSteps);

// Writes a full numeric LUT CSV, a binary PPM visualization, dedicated
// zenith/horizon probes, and a reproducibility manifest. The output is intended
// for deterministic headless qualification and for comparing future GPU LUTs.
bool writeTransmittanceReference(
    const std::filesystem::path &outputDirectory,
    const AtmosphereProfile &profile,
    const AtmosphereReferenceExportSettings &settings,
    std::string *error = nullptr);

} // namespace atmosphere
