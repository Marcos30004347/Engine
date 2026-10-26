#include "atmosphere/AtmosphereDiagnostics.hpp"
#include "atmosphere/AtmosphereReference.hpp"
#include "atmosphere/AtmosphereTypes.hpp"
#include "atmosphere/rendering/AtmosphereLutDebugPass.hpp"
#include "atmosphere/rendering/AtmosphereGPU.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{

int failures = 0;

void check(const bool condition, const char *message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ")
            << message << '\n';
  failures += condition ? 0 : 1;
}

bool near(
    const double left, const double right,
    const double absolute = 1.0e-9,
    const double relative = 1.0e-9)
{
  return std::abs(left - right) <=
      absolute + relative * std::max(std::abs(left), std::abs(right));
}

bool finiteUnit(const atmosphere::Vec3d &value)
{
  for (const double component : value)
    if (!std::isfinite(component) || component < 0.0 || component > 1.0)
      return false;
  return true;
}

double directionAngleDegrees(
    const atmosphere::Vec3d &left, const atmosphere::Vec3d &right)
{
  const double dot = std::clamp(left[0] * right[0] +
      left[1] * right[1] + left[2] * right[2], -1.0, 1.0);
  return std::acos(dot) * (180.0 / 3.14159265358979323846);
}

void testProfile()
{
  const atmosphere::AtmosphereProfile earth =
      atmosphere::earthAtmosphereProfile();
  std::string error;
  check(atmosphere::validateAtmosphereProfile(earth, &error) && error.empty(),
      "Earth profile is valid");
  check(earth.groundRadiusMetres == 6'360'000.0 &&
          earth.topRadiusMetres == 6'460'000.0 &&
          near(earth.rayleighScatteringPerMetre[0], 5.802e-6, 1.0e-12) &&
          near(earth.ozoneAbsorptionPerMetre[1], 1.881e-6, 1.0e-12),
      "Earth preset matches Hillaire 2020 radii and coefficients");
  atmosphere::AtmosphereProfile invalid = earth;
  invalid.topRadiusMetres = invalid.groundRadiusMetres;
  check(!atmosphere::validateAtmosphereProfile(invalid, &error) &&
          !error.empty(),
      "invalid radius contract is rejected with an error");
  check(std::string(atmosphere::kAtmosphereLengthUnit) == "metres" &&
          std::string(atmosphere::kAtmosphereLightDirectionConvention) ==
              "direction light travels",
      "coordinate and light-direction conventions are explicit");
}

void testReferenceMath()
{
  const atmosphere::RaySphereInterval centre =
      atmosphere::intersectRaySphere(
          {0.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, 4.0);
  check(centre.hit && near(centre.nearDistance, -2.0) &&
          near(centre.farDistance, 2.0),
      "ray/sphere intersection supports non-unit directions");
  const atmosphere::RaySphereInterval miss =
      atmosphere::intersectRaySphere(
          {0.0, 5.0, 0.0}, {1.0, 0.0, 0.0}, 4.0);
  check(!miss.hit, "ray/sphere intersection reports misses");

  const atmosphere::AtmosphereProfile earth =
      atmosphere::earthAtmosphereProfile();
  const auto sea = atmosphere::atmosphereDensity(earth, 0.0);
  const auto rayleighScale = atmosphere::atmosphereDensity(earth, 8'000.0);
  const auto ozoneCentre = atmosphere::atmosphereDensity(earth, 25'000.0);
  const auto ozoneEdge = atmosphere::atmosphereDensity(earth, 40'000.0);
  check(near(sea.rayleigh, 1.0) && near(sea.mie, 1.0) &&
          near(rayleighScale.rayleigh, std::exp(-1.0), 1.0e-12) &&
          near(ozoneCentre.ozone, 1.0) && near(ozoneEdge.ozone, 0.0),
      "Rayleigh, Mie, and ozone density profiles match their contracts");

  bool mappingRoundTrips = true;
  for (uint32_t y = 0u; y < 11u; ++y)
    for (uint32_t x = 0u; x < 17u; ++x)
    {
      const std::array<double, 2> uv{
          (static_cast<double>(x) + 0.5) / 17.0,
          (static_cast<double>(y) + 0.5) / 11.0};
      const auto parameters =
          atmosphere::transmittanceLutParameters(earth, uv);
      const auto reconstructed =
          atmosphere::transmittanceLutUv(earth, parameters);
      mappingRoundTrips &= near(uv[0], reconstructed[0], 2.0e-9) &&
          near(uv[1], reconstructed[1], 2.0e-9);
    }
  check(mappingRoundTrips,
      "transmittance LUT mapping round-trips across texel centres");

  bool skyMappingRoundTrips = true;
  for (uint32_t y = 0u; y < 11u; ++y)
    for (uint32_t x = 0u; x < 17u; ++x)
    {
      const std::array<double, 2> uv{
          (static_cast<double>(x) + 0.5) / 17.0,
          (static_cast<double>(y) + 0.5) / 11.0};
      const auto reconstructed = atmosphere::skyViewUv(
          atmosphere::skyViewDirection(uv));
      skyMappingRoundTrips &= near(uv[0], reconstructed[0], 2.0e-9) &&
          near(uv[1], reconstructed[1], 2.0e-9);
    }
  check(skyMappingRoundTrips,
      "nonlinear Sky-View mapping round-trips across texel centres");

  constexpr double aerialMaximumDistance = 320'000.0;
  constexpr double aerialDistributionScale = 100.0;
  bool aerialMappingRoundTrips = true;
  double previousAerialDistance = -1.0;
  for (uint32_t slice = 0u; slice <= 32u; ++slice)
  {
    const double normalizedDepth = static_cast<double>(slice) / 32.0;
    const double distance = atmosphere::aerialPerspectiveDistanceMetres(
        normalizedDepth, aerialMaximumDistance, aerialDistributionScale);
    const double reconstructed =
        atmosphere::aerialPerspectiveNormalizedDepth(distance,
            aerialMaximumDistance, aerialDistributionScale);
    aerialMappingRoundTrips &= near(normalizedDepth, reconstructed, 2.0e-12) &&
        distance >= previousAerialDistance;
    previousAerialDistance = distance;
  }
  check(aerialMappingRoundTrips &&
          near(atmosphere::aerialPerspectiveDistanceMetres(0.0,
                   aerialMaximumDistance, aerialDistributionScale), 0.0) &&
          near(atmosphere::aerialPerspectiveDistanceMetres(1.0,
                   aerialMaximumDistance, aerialDistributionScale),
              aerialMaximumDistance, 1.0e-7),
      "logarithmic aerial-perspective depth is monotonic and invertible");

  const std::vector<atmosphere::Vec3d> shDirections{
    {1.0, 0.0, 0.0}, {-1.0, 0.0, 0.0},
    {0.0, 1.0, 0.0}, {0.0, -1.0, 0.0},
    {0.0, 0.0, 1.0}, {0.0, 0.0, -1.0}};
  const atmosphere::Vec3d constantSky{0.4, 0.7, 1.1};
  const std::vector<atmosphere::Vec3d> shRadiances(
      shDirections.size(), constantSky);
  const atmosphere::AtmosphereAmbientSH1 sh =
      atmosphere::projectAtmosphereAmbientSH1(
          shDirections, shRadiances);
  const atmosphere::Vec3d reconstructedSky =
      atmosphere::evaluateAtmosphereAmbientSH1(sh, {0.3, 0.8, -0.2});
  const atmosphere::Vec3d meanSky =
      atmosphere::meanAtmosphereAmbientRadiance(sh);
  check(near(reconstructedSky[0], constantSky[0], 1.0e-12) &&
          near(reconstructedSky[1], constantSky[1], 1.0e-12) &&
          near(reconstructedSky[2], constantSky[2], 1.0e-12) &&
          near(meanSky[0], constantSky[0], 1.0e-12) &&
          near(meanSky[1], constantSky[1], 1.0e-12) &&
          near(meanSky[2], constantSky[2], 1.0e-12),
      "first-order atmosphere SH preserves constant sky radiance and DC mean");

  atmosphere::SunSettings revisionSun{};
  revisionSun.directionPlanetSpace = {1.0, 0.0, 0.0};
  const auto baseSunKey = atmosphere::quantizedSunRevisionKey(revisionSun);
  revisionSun.directionPlanetSpace = {
      std::cos(0.1 * 3.14159265358979323846 / 180.0), 0.0,
      std::sin(0.1 * 3.14159265358979323846 / 180.0)};
  const auto subThresholdSunKey =
      atmosphere::quantizedSunRevisionKey(revisionSun);
  revisionSun.directionPlanetSpace = {
      std::cos(0.3 * 3.14159265358979323846 / 180.0), 0.0,
      std::sin(0.3 * 3.14159265358979323846 / 180.0)};
  const auto crossedSunKey = atmosphere::quantizedSunRevisionKey(revisionSun);
  check(baseSunKey == subThresholdSunKey &&
          !(baseSunKey == crossedSunKey),
      "smooth sun identity changes only after the 0.25 degree refresh threshold");

  // Model sixty seconds of accelerated motion at 60 Hz. The cache key should
  // advance progressively as angular bins are crossed, never once per frame.
  uint32_t progressiveRefreshes = 0u;
  auto previousSunKey = baseSunKey;
  for (uint32_t frame = 1u; frame <= 3'600u; ++frame)
  {
    const double degrees = 6.0 * static_cast<double>(frame) / 3'600.0;
    const double radians = degrees * 3.14159265358979323846 / 180.0;
    revisionSun.directionPlanetSpace =
        {std::cos(radians), 0.0, std::sin(radians)};
    const auto currentSunKey =
        atmosphere::quantizedSunRevisionKey(revisionSun);
    if (!(currentSunKey == previousSunKey))
    {
      ++progressiveRefreshes;
      previousSunKey = currentSunKey;
    }
  }
  check(progressiveRefreshes >= 20u && progressiveRefreshes <= 30u,
      "sixty seconds of smooth sun motion triggers progressive refreshes, not per-frame resets");

  const double radius = earth.groundRadiusMetres + 2.0;
  const auto zenith = atmosphere::referenceTransmittanceToAtmosphereTop(
      earth, radius, 1.0, 1024u);
  const auto horizon = atmosphere::referenceTransmittanceToAtmosphereTop(
      earth, radius, 0.0, 2048u);
  check(finiteUnit(zenith) && finiteUnit(horizon) &&
          horizon[0] < zenith[0] && horizon[1] < zenith[1] &&
          horizon[2] < zenith[2],
      "Earth horizon has more optical depth than zenith");

  const atmosphere::Vec3d origin{0.0, radius, 0.0};
  const atmosphere::Vec3d direction{0.0, 1.0, 0.0};
  const auto shortPath = atmosphere::transmittanceAlongSegment(
      earth, origin, direction, 10'000.0, 256u);
  const auto longPath = atmosphere::transmittanceAlongSegment(
      earth, origin, direction, 50'000.0, 1024u);
  check(longPath[0] <= shortPath[0] && longPath[1] <= shortPath[1] &&
          longPath[2] <= shortPath[2],
      "transmittance is monotonic with optical path length");

  const atmosphere::AtmosphereProfile vacuum =
      atmosphere::vacuumAtmosphereProfile(1'000.0);
  const auto vacuumValue = atmosphere::referenceTransmittanceToAtmosphereTop(
      vacuum, 1'000.0, 1.0, 8u);
  check(near(vacuumValue[0], 1.0) && near(vacuumValue[1], 1.0) &&
          near(vacuumValue[2], 1.0),
      "vacuum reference has identity transmittance");
}

void testDayNightCycle()
{
  atmosphere::DayNightCycleSettings cycle{};
  cycle.enabled = true;
  cycle.timeOfDayHours = 12.0;
  cycle.dayOfYear = 0.0;
  cycle.latitudeDegrees = 0.0;
  cycle.longitudeDegrees = 0.0;
  cycle.axialTiltDegrees = 23.4393;
  const auto noon = atmosphere::evaluateDayNightSun(cycle);
  cycle.timeOfDayHours = 0.0;
  const auto midnight = atmosphere::evaluateDayNightSun(cycle);
  check(near(noon.directionTowardSun[1], 1.0, 1.0e-12) &&
          near(midnight.directionTowardSun[1], -1.0, 1.0e-12) &&
          near(noon.directionLightTravels[1], -1.0, 1.0e-12),
      "celestial controller agrees at equatorial equinox noon and midnight");

  cycle.mode = atmosphere::DayNightCycleMode::Manual;
  cycle.manualAzimuthDegrees = 90.0;
  cycle.manualElevationDegrees = 30.0;
  const auto manual = atmosphere::evaluateDayNightSun(cycle);
  check(near(manual.directionTowardSun[0], std::cos(30.0 *
                 3.14159265358979323846 / 180.0), 1.0e-12) &&
          near(manual.directionTowardSun[1], 0.5, 1.0e-12) &&
          near(manual.directionTowardSun[2], 0.0, 1.0e-12),
      "manual sun mode uses north-zero local azimuth and horizon elevation");

  cycle = {};
  cycle.enabled = true;
  cycle.dayLengthSeconds = 60.0;
  cycle.timeOfDayHours = 23.5;
  cycle.dayOfYear = cycle.orbitalPeriodDays - 0.25;
  atmosphere::advanceDayNightCycle(cycle, 2.5);
  check(near(cycle.timeOfDayHours, 0.5, 1.0e-12) &&
          near(cycle.dayOfYear, 0.75, 1.0e-12),
      "double-precision clock wraps time of day and orbital year together");

  cycle.timeOfDayHours = 0.0;
  cycle.dayOfYear = 80.0;
  const auto first = atmosphere::evaluateDayNightSun(cycle);
  auto previous = first;
  double maximumStepDegrees = 0.0;
  uint32_t progressiveKeys = 0u;
  atmosphere::SunSettings keySun{};
  keySun.directionPlanetSpace = first.directionLightTravels;
  auto previousKey = atmosphere::quantizedSunRevisionKey(keySun);
  for (uint32_t frame = 0u; frame < 3'600u; ++frame)
  {
    atmosphere::advanceDayNightCycle(cycle, 1.0 / 60.0);
    const auto current = atmosphere::evaluateDayNightSun(cycle);
    maximumStepDegrees = std::max(maximumStepDegrees,
        directionAngleDegrees(previous.directionTowardSun,
            current.directionTowardSun));
    keySun.directionPlanetSpace = current.directionLightTravels;
    const auto key = atmosphere::quantizedSunRevisionKey(keySun);
    if (!(key == previousKey))
    {
      ++progressiveKeys;
      previousKey = key;
    }
    previous = current;
  }
  const auto last = atmosphere::evaluateDayNightSun(cycle);
  check(maximumStepDegrees < 0.2 &&
          directionAngleDegrees(first.directionTowardSun,
              last.directionTowardSun) < 1.0 &&
          progressiveKeys > 100u && progressiveKeys < 3'600u,
      "accelerated 24-hour cycle is continuous and refreshes quantized GI progressively");
}

void testOrbitAtmosphereContracts()
{
  constexpr double atmosphereHeight = 100'000.0;
  atmosphere::AtmosphereViewMode mode = atmosphere::AtmosphereViewMode::Ground;
  mode = atmosphere::updateAtmosphereViewMode(
      mode, 19'999.0, atmosphereHeight);
  check(mode == atmosphere::AtmosphereViewMode::Ground,
      "orbit atmosphere remains in ground mode below its enter threshold");
  mode = atmosphere::updateAtmosphereViewMode(
      mode, 20'001.0, atmosphereHeight);
  check(mode == atmosphere::AtmosphereViewMode::Orbit,
      "orbit atmosphere enters above twenty percent of atmosphere height");
  mode = atmosphere::updateAtmosphereViewMode(
      mode, 17'500.0, atmosphereHeight);
  check(mode == atmosphere::AtmosphereViewMode::Orbit,
      "orbit atmosphere hysteresis retains orbit mode inside the dead band");
  mode = atmosphere::updateAtmosphereViewMode(
      mode, 14'999.0, atmosphereHeight);
  check(mode == atmosphere::AtmosphereViewMode::Ground,
      "orbit atmosphere leaves below fifteen percent of atmosphere height");

  const auto nearBounds = atmosphere::projectedAtmosphereScreenBounds(
      1920u, 1080u, 60.0 * 3.14159265358979323846 / 180.0,
      2.2, 6'460.0 / 6'360.0);
  const auto farBounds = atmosphere::projectedAtmosphereScreenBounds(
      1920u, 1080u, 60.0 * 3.14159265358979323846 / 180.0,
      6.0, 6'460.0 / 6'360.0);
  const auto insideBounds = atmosphere::projectedAtmosphereScreenBounds(
      320u, 180u, 60.0 * 3.14159265358979323846 / 180.0,
      1.01, 6'460.0 / 6'360.0);
  check(nearBounds.valid() && farBounds.valid() &&
          nearBounds.width > farBounds.width &&
          nearBounds.height > farBounds.height &&
          nearBounds.x + nearBounds.width <= 1920u &&
          nearBounds.y + nearBounds.height <= 1080u,
      "projected atmosphere dispatch bounds shrink with orbit distance and stay clipped");
  check(insideBounds.x == 0u && insideBounds.y == 0u &&
          insideBounds.width == 320u && insideBounds.height == 180u,
      "camera positions inside the shell conservatively cover the viewport");

  const atmosphere::AtmosphereProfile earth =
      atmosphere::earthAtmosphereProfile();
  const atmosphere::Vec3d camera{0.0, 0.0,
      earth.groundRadiusMetres * 2.2};
  const auto atmosphereHit = atmosphere::intersectRaySphere(
      camera, {0.0, 0.0, -1.0}, earth.topRadiusMetres);
  const auto groundHit = atmosphere::intersectRaySphere(
      camera, {0.0, 0.0, -1.0}, earth.groundRadiusMetres);
  const auto miss = atmosphere::intersectRaySphere(
      camera, {0.0, 1.0, 0.0}, earth.topRadiusMetres);
  check(atmosphereHit.hit && groundHit.hit &&
          atmosphereHit.nearDistance < groundHit.nearDistance &&
          atmosphereHit.farDistance > groundHit.farDistance,
      "orbit centre ray enters atmosphere before the surface and exits behind it");
  check(!miss.hit,
      "orbit ray outside the projected shell leaves empty space untouched");
}

void testTerrainVolumetricShadowContracts()
{
  check(near(atmosphere::terrainShadowCoverageWeight(
                 false, 1.0, 0.0), 0.0) &&
          near(atmosphere::terrainShadowCoverageWeight(
                 true, -0.1, 0.0), 0.0),
      "terrain atmosphere shadows fall back to PlanetOnly without coverage or sun");
  const float centre = atmosphere::terrainShadowCoverageWeight(
      true, 0.5, 0.0);
  const float middle = atmosphere::terrainShadowCoverageWeight(
      true, 0.08, 0.5);
  const float edge = atmosphere::terrainShadowCoverageWeight(
      true, 0.5, 1.0);
  check(near(centre, 1.0) && middle > 0.0f && middle < 1.0f &&
          near(edge, 0.0),
      "terrain atmosphere shadow coverage fades at low sun and map edges");

  atmosphere::SunSettings sun{};
  const auto first = atmosphere::quantizedSunRevisionKey(sun, 0.25, 1.0);
  check(atmosphere::terrainShadowMapNeedsUpdate(
            {}, first, false, false, true) &&
          !atmosphere::terrainShadowMapNeedsUpdate(
            first, first, true, false, true) &&
          atmosphere::terrainShadowMapNeedsUpdate(
            first, first, true, true, true) &&
          !atmosphere::terrainShadowMapNeedsUpdate(
            first, first, true, true, false),
      "terrain shadow map updates on first coverage and anchor changes only when available");
  sun.directionPlanetSpace = {0.01, -0.99995, 0.0};
  const auto moved = atmosphere::quantizedSunRevisionKey(sun, 0.25, 1.0);
  check(atmosphere::terrainShadowMapNeedsUpdate(
            first, moved, true, false, true),
      "terrain shadow map updates after the sun crosses its quarter-degree bin");
}

void testDirtyRules()
{
  atmosphere::AtmosphereLutRebuildLatch rebuildLatch{};
  rebuildLatch.arm(2u);
  const bool firstRebuild = rebuildLatch.consume();
  rebuildLatch.arm(1u);
  const bool secondRebuild = rebuildLatch.consume();
  const bool thirdRebuild = rebuildLatch.consume();
  check(firstRebuild && secondRebuild && thirdRebuild &&
          !rebuildLatch.consume(),
      "static LUT rebuild requests survive the complete frames-in-flight window");

  atmosphere::AtmosphereState before{};
  atmosphere::AtmosphereState after = before;
  check(!atmosphere::any(
          atmosphere::classifyAtmosphereChanges(before, after)),
      "unchanged state leaves every LUT clean");

  after.sun.directionPlanetSpace = {1.0, 0.0, 0.0};
  auto dirty = atmosphere::classifyAtmosphereChanges(before, after);
  check(!atmosphere::hasFlag(dirty,
            atmosphere::AtmosphereDirtyFlags::Transmittance) &&
          !atmosphere::hasFlag(dirty,
            atmosphere::AtmosphereDirtyFlags::MultipleScattering) &&
          atmosphere::hasFlag(dirty,
            atmosphere::AtmosphereDirtyFlags::SkyView) &&
          atmosphere::hasFlag(dirty,
            atmosphere::AtmosphereDirtyFlags::AerialPerspective) &&
          atmosphere::hasFlag(dirty,
            atmosphere::AtmosphereDirtyFlags::Ambient),
      "sun motion dirties only camera/sun-dependent products");

  after = before;
  after.view.viewportWidth = 1920u;
  dirty = atmosphere::classifyAtmosphereChanges(before, after);
  check(dirty == atmosphere::AtmosphereDirtyFlags::AerialPerspective,
      "viewport changes dirty only frustum aerial perspective");

  after = before;
  after.profile.groundAlbedo[0] = 0.2f;
  dirty = atmosphere::classifyAtmosphereChanges(before, after);
  check(!atmosphere::hasFlag(dirty,
            atmosphere::AtmosphereDirtyFlags::Transmittance) &&
          atmosphere::hasFlag(dirty,
            atmosphere::AtmosphereDirtyFlags::MultipleScattering),
      "ground albedo rebuilds scattering but not transmittance");

  after = before;
  after.profile.rayleighScaleHeightMetres += 1.0f;
  dirty = atmosphere::classifyAtmosphereChanges(before, after);
  check(dirty == atmosphere::AtmosphereDirtyFlags::All,
      "optical-profile edits dirty the complete LUT chain");

  atmosphere::AtmosphereRevisions revisions{};
  atmosphere::advanceAtmosphereRevisions(revisions, dirty, false);
  check(revisions.transmittance == 2u &&
          revisions.multipleScattering == 2u &&
          revisions.skyView == 2u && revisions.aerialPerspective == 2u &&
          revisions.ambient == 2u && revisions.profile == 2u &&
          revisions.discontinuityEpoch == 1u,
      "dirty products advance independently without forcing discontinuity");
  atmosphere::advanceAtmosphereRevisions(
      revisions, atmosphere::AtmosphereDirtyFlags::None, true);
  check(revisions.discontinuityEpoch == 2u &&
          revisions.skyView == 2u,
      "explicit discontinuity epoch is separate from smooth LUT revisions");
}

void testDiagnostics()
{
  check(static_cast<uint32_t>(
            atmosphere::rendering::AtmosphereLutDebugPass::View::
                AerialPerspectiveTransmittance) == 5u,
      "generic LUT presenter exposes 2D and 3D aerial debug views");
  check(atmosphere::AtmosphereDiagnostics::classify(
            "svr.sample.Atmosphere.AerialPerspective") ==
          atmosphere::AtmospherePassKind::AerialPerspective,
      "diagnostics classify the aerial-perspective GPU pass");
  check(atmosphere::AtmosphereDiagnostics::classify(
            "svr.sample.Atmosphere.Ambient") ==
          atmosphere::AtmospherePassKind::Ambient,
      "diagnostics classify the ambient SH projection GPU pass");
  check(atmosphere::AtmosphereDiagnostics::classify(
            "svr.sample.Atmosphere.TerrainShadow") ==
          atmosphere::AtmospherePassKind::TerrainShadow &&
          atmosphere::AtmosphereDiagnostics::classify(
            "svr.sample.Atmosphere.SkyView") ==
          atmosphere::AtmospherePassKind::SkyView &&
          atmosphere::AtmosphereDiagnostics::classify(
            "svr.sample.Atmosphere.Overview") ==
          atmosphere::AtmospherePassKind::Overview &&
          atmosphere::AtmosphereDiagnostics::classify(
            "svr.sample.Atmosphere.LutDebug") ==
          atmosphere::AtmospherePassKind::Debug,
      "diagnostics classify terrain, sky, orbit, and generic debug passes");
  atmosphere::AtmosphereDiagnostics diagnostics;
  std::vector<rendering::RenderGraph::TimerReadbackRecord> records{
    {.passName = "atmosphere.Transmittance", .timerName = "main",
     .passIndex = 1u, .gpuTimeMs = 0.10,
     .hasComputeWork = true, .hasGraphicsWork = false},
    {.passName = "svr.sample.AtmosphereComposite", .timerName = "main",
     .passIndex = 2u, .gpuTimeMs = 0.20,
     .hasComputeWork = true, .hasGraphicsWork = false},
    {.passName = "svr.sample.GBuffer", .timerName = "main",
     .passIndex = 3u, .gpuTimeMs = 3.00,
     .hasComputeWork = false, .hasGraphicsWork = true},
  };
  diagnostics.pushFrame(7u, records);
  diagnostics.pushFrame(7u, records);
  const auto total = diagnostics.totalStatistics();
  const auto transmittance = diagnostics.statistics(
      atmosphere::AtmospherePassKind::Transmittance);
  check(diagnostics.frameCount() == 1u && near(total.latestMs, 0.30) &&
          near(transmittance.latestMs, 0.10),
      "diagnostics aggregate atmosphere timers and reject duplicate frames");
  records[0].gpuTimeMs = 0.30;
  records[1].gpuTimeMs = 0.40;
  diagnostics.pushFrame(8u, records);
  const auto twoFrames = diagnostics.totalStatistics();
  check(twoFrames.sampleCount == 2u && near(twoFrames.medianMs, 0.30) &&
          near(twoFrames.percentile95Ms, 0.70),
      "diagnostics publish latest, median, and p95 timing statistics");

  atmosphere::AtmosphereDiagnostics rolling;
  const std::vector<rendering::RenderGraph::TimerReadbackRecord> onePass{
    {.passName = "atmosphere.TerrainShadow", .timerName = "main",
     .passIndex = 1u, .gpuTimeMs = 0.05,
     .hasComputeWork = false, .hasGraphicsWork = true},
  };
  for (uint64_t frame = 0u;
       frame < atmosphere::AtmosphereDiagnostics::kHistoryLength + 20u;
       ++frame)
    rolling.pushFrame(frame, onePass);
  const auto rollingTerrain = rolling.statistics(
      atmosphere::AtmospherePassKind::TerrainShadow);
  check(rolling.frameCount() ==
            atmosphere::AtmosphereDiagnostics::kHistoryLength &&
          rollingTerrain.sampleCount ==
            atmosphere::AtmosphereDiagnostics::kHistoryLength &&
          near(rollingTerrain.latestMs, 0.05) &&
          near(rollingTerrain.medianMs, 0.05) &&
          near(rollingTerrain.percentile95Ms, 0.05),
      "diagnostic history remains bounded while preserving terrain timing statistics");
}

void testGpuContract()
{
  atmosphere::SunSettings sun{};
  sun.directionPlanetSpace = {0.0, -2.0, 0.0};
  sun.intensity = 3.0f;
  const auto parameters = atmosphere::rendering::makeAtmosphereParametersGPU(
      atmosphere::earthAtmosphereProfile(), sun);
  check(atmosphere::rendering::finiteAtmosphereParametersGPU(parameters) &&
          near(parameters.radiiMieG[0], 6360.0, 1.0e-2) &&
          near(parameters.radiiMieG[1], 6460.0, 1.0e-2) &&
          near(parameters.rayleighScatteringScale[0], 0.005802,
              1.0e-7) &&
          near(parameters.sunDirectionIntensity[1], -1.0, 1.0e-7) &&
          near(parameters.sunDirectionIntensity[3], 3.0, 1.0e-7),
      "GPU contract converts metres to kilometres and normalizes the sun");
}

void testReferenceExport()
{
  const std::filesystem::path output =
      std::filesystem::temp_directory_path() /
      "engine-atmosphere-phase0-reference";
  std::error_code ignored;
  std::filesystem::remove_all(output, ignored);
  std::string error;
  const bool written = atmosphere::writeTransmittanceReference(
      output, atmosphere::earthAtmosphereProfile(),
      {.width = 8u, .height = 4u, .integrationSteps = 32u}, &error);
  std::ifstream csv(output / "transmittance.csv");
  std::ifstream probes(output / "transmittance-probes.csv");
  std::ifstream ppm(output / "transmittance.ppm", std::ios::binary);
  std::ifstream manifest(output / "reference.txt");
  std::string csvHeader;
  std::string probesText(
      (std::istreambuf_iterator<char>(probes)),
      std::istreambuf_iterator<char>());
  std::string ppmMagic;
  std::string manifestText(
      (std::istreambuf_iterator<char>(manifest)),
      std::istreambuf_iterator<char>());
  std::getline(csv, csvHeader);
  std::getline(ppm, ppmMagic);
  check(written && error.empty() &&
          csvHeader.find("altitude_metres") != std::string::npos &&
          probesText.find("0,zenith,1,") != std::string::npos &&
          probesText.find("90000,horizon,0,") != std::string::npos &&
          ppmMagic == "P6" &&
          manifestText.find("model=Hillaire2020") != std::string::npos,
      "headless harness writes LUT data, altitude probes, PPM, and manifest");
  std::filesystem::remove_all(output, ignored);
}

} // namespace

int main()
{
  std::cout << "atmosphere phase 0/1/2/3/4/5/6 contracts\n";
  testProfile();
  testReferenceMath();
  testDayNightCycle();
  testOrbitAtmosphereContracts();
  testTerrainVolumetricShadowContracts();
  testDirtyRules();
  testDiagnostics();
  testGpuContract();
  testReferenceExport();
  if (failures != 0)
    std::cerr << failures << " atmosphere contract checks failed\n";
  return failures == 0 ? 0 : 1;
}
