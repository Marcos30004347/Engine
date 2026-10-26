#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <string>

namespace atmosphere
{

// CPU atmosphere contracts use metres and double-precision planet-relative
// positions. GPU passes may normalize or convert them, but that conversion must
// happen once when AtmosphereFrameState is assembled.
inline constexpr const char *kAtmosphereLengthUnit = "metres";
inline constexpr const char *kAtmosphereLightDirectionConvention =
    "direction light travels";

using Vec3d = std::array<double, 3>;
using Vec3f = std::array<float, 3>;

struct AtmosphereProfile
{
  double groundRadiusMetres = 6'360'000.0;
  double topRadiusMetres = 6'460'000.0;
  Vec3f rayleighScatteringPerMetre{5.802e-6f, 13.558e-6f, 33.1e-6f};
  float rayleighScaleHeightMetres = 8'000.0f;
  Vec3f mieScatteringPerMetre{3.996e-6f, 3.996e-6f, 3.996e-6f};
  Vec3f mieAbsorptionPerMetre{4.40e-6f, 4.40e-6f, 4.40e-6f};
  float mieScaleHeightMetres = 1'200.0f;
  float mieAnisotropy = 0.8f;
  Vec3f ozoneAbsorptionPerMetre{0.650e-6f, 1.881e-6f, 0.085e-6f};
  float ozoneCentreMetres = 25'000.0f;
  float ozoneHalfWidthMetres = 15'000.0f;
  Vec3f groundAlbedo{0.3f, 0.3f, 0.3f};
  float atmosphereRadianceScale = 1.0f;
};

struct SunSettings
{
  bool enabled = true;
  // Planet-space direction in which sunlight travels, matching WorldGILight.
  Vec3d directionPlanetSpace{0.0, -1.0, 0.0};
  Vec3f color{1.0f, 1.0f, 1.0f};
  float intensity = 1.0f;
  float angularRadiusRadians = 0.004675f;
  bool drawDisk = true;
  float diskLuminanceScale = 1.0f;
};

enum class DayNightCycleMode : uint32_t
{
  Celestial = 0u,
  Manual = 1u,
};

// Double-precision world time and the small celestial approximation used to
// drive the one shared SunSettings record. Manual mode exposes a direct local
// azimuth/elevation pair without changing any rendering-pass contract.
struct DayNightCycleSettings
{
  bool enabled = false;
  bool paused = false;
  DayNightCycleMode mode = DayNightCycleMode::Celestial;
  double timeOfDayHours = 13.5;
  double dayLengthSeconds = 600.0;
  double dayOfYear = 80.0;
  double orbitalPeriodDays = 365.2422;
  double axialTiltDegrees = 23.4393;
  double latitudeDegrees = 35.0;
  double longitudeDegrees = 0.0;
  double timeScale = 1.0;
  // Toward-sun angles in the local tangent frame: zero azimuth is north (+Z),
  // 90 degrees is east (+X), and elevation is measured above the horizon.
  double manualAzimuthDegrees = 225.0;
  double manualElevationDegrees = 60.0;
};

struct DayNightSunState
{
  // Both are unit vectors in the local tangent/world frame.
  Vec3d directionTowardSun{0.0, 1.0, 0.0};
  Vec3d directionLightTravels{0.0, -1.0, 0.0};
  double elevationDegrees = 90.0;
  double declinationDegrees = 0.0;
  double hourAngleDegrees = 0.0;
};

enum class AtmosphereViewMode : uint32_t
{
  Ground = 0u,
  Orbit = 1u,
};

enum class AtmosphereVolumetricShadowMode : uint32_t
{
  PlanetOnly = 0u,
  TerrainApproximation = 1u,
};

struct AtmosphereScreenBounds
{
  uint32_t x = 0u;
  uint32_t y = 0u;
  uint32_t width = 0u;
  uint32_t height = 0u;

  bool valid() const { return width != 0u && height != 0u; }
};

struct AtmosphereViewState
{
  Vec3d cameraPlanetPositionMetres{0.0, 6'360'002.0, 0.0};
  Vec3d localUpPlanetSpace{0.0, 1.0, 0.0};
  double verticalFovRadians = 1.0471975511965976;
  uint32_t viewportWidth = 1u;
  uint32_t viewportHeight = 1u;
};

enum class AtmosphereDirtyFlags : uint32_t
{
  None = 0u,
  Transmittance = 1u << 0u,
  MultipleScattering = 1u << 1u,
  SkyView = 1u << 2u,
  AerialPerspective = 1u << 3u,
  Ambient = 1u << 4u,
  All = Transmittance | MultipleScattering | SkyView |
      AerialPerspective | Ambient,
};

constexpr AtmosphereDirtyFlags operator|(
    const AtmosphereDirtyFlags left, const AtmosphereDirtyFlags right)
{
  return static_cast<AtmosphereDirtyFlags>(
      static_cast<uint32_t>(left) | static_cast<uint32_t>(right));
}

constexpr AtmosphereDirtyFlags operator&(
    const AtmosphereDirtyFlags left, const AtmosphereDirtyFlags right)
{
  return static_cast<AtmosphereDirtyFlags>(
      static_cast<uint32_t>(left) & static_cast<uint32_t>(right));
}

inline AtmosphereDirtyFlags &operator|=(
    AtmosphereDirtyFlags &left, const AtmosphereDirtyFlags right)
{
  left = left | right;
  return left;
}

constexpr bool any(const AtmosphereDirtyFlags value)
{
  return value != AtmosphereDirtyFlags::None;
}

constexpr bool hasFlag(
    const AtmosphereDirtyFlags value, const AtmosphereDirtyFlags flag)
{
  return any(value & flag);
}

struct AtmosphereState
{
  AtmosphereProfile profile{};
  SunSettings sun{};
  AtmosphereViewState view{};
};

struct AtmosphereRevisions
{
  uint64_t profile = 1u;
  uint64_t transmittance = 1u;
  uint64_t multipleScattering = 1u;
  uint64_t skyView = 1u;
  uint64_t aerialPerspective = 1u;
  uint64_t ambient = 1u;
  // Changes only for teleports, profile switches, manual time scrubs,
  // enable/disable transitions, or other discontinuous lighting changes.
  uint64_t discontinuityEpoch = 1u;
};

// A persistent LUT rebuild is requested by the CPU but observed by
// asynchronously submitted GPU work. Keep that request alive for one more
// update than the maximum number of frames in flight, preventing a later
// uniform upload from clearing the bit before any dispatch consumes it.
struct AtmosphereLutRebuildLatch
{
  void arm(const uint32_t maximumFramesInFlight)
  {
    const uint32_t requested = maximumFramesInFlight ==
            std::numeric_limits<uint32_t>::max()
        ? maximumFramesInFlight
        : maximumFramesInFlight + 1u;
    if (requested > framesRemaining)
      framesRemaining = requested;
  }

  bool consume()
  {
    if (framesRemaining == 0u)
      return false;
    --framesRemaining;
    return true;
  }

  uint32_t framesRemaining = 0u;
};

struct QuantizedSunRevisionKey
{
  int32_t azimuthBin = 0;
  int32_t elevationBin = 0;
  std::array<int32_t, 3> radianceBins{};
  bool enabled = false;

  bool operator==(const QuantizedSunRevisionKey &other) const
  {
    return azimuthBin == other.azimuthBin &&
        elevationBin == other.elevationBin &&
        radianceBins == other.radianceBins && enabled == other.enabled;
  }
};

AtmosphereProfile earthAtmosphereProfile();
AtmosphereProfile vacuumAtmosphereProfile(double groundRadiusMetres);
bool validateAtmosphereProfile(
    const AtmosphereProfile &profile, std::string *error = nullptr);

DayNightCycleSettings sanitizeDayNightCycleSettings(
    DayNightCycleSettings settings);
void advanceDayNightCycle(
    DayNightCycleSettings &settings, double realDeltaSeconds);
DayNightSunState evaluateDayNightSun(
    const DayNightCycleSettings &settings);
bool dayNightCycleEqual(
    const DayNightCycleSettings &left,
    const DayNightCycleSettings &right);

// Stateful ground/orbit selection. Separate enter/leave thresholds prevent
// ascent and descent from flickering around the atmosphere handoff altitude.
AtmosphereViewMode updateAtmosphereViewMode(
    AtmosphereViewMode current,
    double altitudeMetres,
    double atmosphereHeightMetres,
    double orbitEnterFraction = 0.20,
    double orbitLeaveFraction = 0.15);

// Conservative, centred pixel bounds of a spherical atmosphere shell viewed
// by the planet-overview camera. cameraDistanceRadii and topRadiusRadii use
// ground-radius units.
AtmosphereScreenBounds projectedAtmosphereScreenBounds(
    uint32_t viewportWidth,
    uint32_t viewportHeight,
    double verticalFovRadians,
    double cameraDistanceRadii,
    double topRadiusRadii,
    uint32_t paddingPixels = 2u);

AtmosphereDirtyFlags classifyAtmosphereChanges(
    const AtmosphereState &previous, const AtmosphereState &current);

void advanceAtmosphereRevisions(
    AtmosphereRevisions &revisions,
    AtmosphereDirtyFlags dirty,
    bool discontinuity);

// Cache/probe identity for smoothly moving sunlight. Direction changes below
// angularStepDegrees and emitted-radiance changes below relativeRadianceStep
// retain their key; discontinuous edits use AtmosphereRevisions instead.
QuantizedSunRevisionKey quantizedSunRevisionKey(const SunSettings &sun,
    double angularStepDegrees = 0.25,
    double relativeRadianceStep = 0.02);

// The optional terrain map is a refinement of PlanetOnly, never a replacement
// for it. A missing cap, invalid extent, or grazing/below-horizon sun therefore
// fades back to one rather than making the atmosphere incorrectly dark.
float terrainShadowCoverageWeight(bool coverageAvailable,
    double sunZenithCosine, double normalizedMapRadius,
    double minimumSunZenithCosine = 0.04,
    double fullSunZenithCosine = 0.12,
    double edgeFadeStart = 0.82);

bool terrainShadowMapNeedsUpdate(const QuantizedSunRevisionKey &previous,
    const QuantizedSunRevisionKey &current, bool hasPrevious,
    bool anchorChanged, bool coverageAvailable);

} // namespace atmosphere
