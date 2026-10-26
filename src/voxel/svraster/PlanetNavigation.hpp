#pragma once

#include "procedural/SphericalVoronoiMesh.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace voxel::svraster
{

enum class PlanetNavigationState
{
  Surface,
  Ascending,
  Orbit,
  LoadingTarget,
  Descending,
};

const char *planetNavigationStateName(PlanetNavigationState state);

struct PlanetNavigationQuaternion
{
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  double w = 1.0;
};

struct PlanetNavigationPose
{
  procedural::SpherePoint direction{};
  double altitudeMetres = 0.0;
  double orbitDistanceRadii = 1.12;
  PlanetNavigationQuaternion orientation{};
};

struct PlanetTangentFrame
{
  procedural::SpherePoint up{};
  procedural::SpherePoint east{1.0, 0.0, 0.0};
  procedural::SpherePoint north{0.0, 1.0, 0.0};
};

struct PlanetSurfaceCameraPose
{
  std::array<double, 3> positionMetres{};
  PlanetNavigationQuaternion orientation{};
};

// Monotonic latest-request-wins gate shared by asynchronous planet navigation
// producers and the render-thread publication point. Cancelling advances the
// generation, so work that cannot stop immediately still cannot publish.
class PlanetNavigationRequestGate
{
public:
  uint64_t submit();
  uint64_t cancel();
  bool accepts(uint64_t generation) const;
  uint64_t generation() const { return generation_; }

private:
  uint64_t advance();
  uint64_t generation_ = 0u;
};

struct PlanetOverviewRay
{
  procedural::SpherePoint origin{};
  procedural::SpherePoint direction{};
};

bool planetOverviewCursorRay(
    double cursorPixelsX,
    double cursorPixelsY,
    uint32_t viewportWidth,
    uint32_t viewportHeight,
    double verticalFovRadians,
    const PlanetTangentFrame &cameraFrame,
    double orbitDistanceRadii,
    PlanetOverviewRay &ray);

bool planetRaySphereIntersection(const PlanetOverviewRay &ray, double sphereRadius, double &distance, procedural::SpherePoint *position = nullptr);

bool planetOverviewPickDirection(
    double cursorPixelsX,
    double cursorPixelsY,
    uint32_t viewportWidth,
    uint32_t viewportHeight,
    double verticalFovRadians,
    const PlanetTangentFrame &cameraFrame,
    double orbitDistanceRadii,
    procedural::SpherePoint &direction);

// Projects a visible unit-sphere direction through the same normalized
// overview camera used by the globe renderer. Hidden-hemisphere and
// off-viewport directions return false so UI labels never float over the wrong
// side of the planet.
bool planetOverviewProjectDirection(
    const procedural::SpherePoint &direction,
    uint32_t viewportWidth,
    uint32_t viewportHeight,
    double verticalFovRadians,
    const PlanetTangentFrame &cameraFrame,
    double orbitDistanceRadii,
    std::array<double, 2> &cursorPixels);

// Returns the requested distance or a farther viewport-aware distance when
// necessary to keep the complete relief globe comfortably inside the screen.
// Rendering, picking, and projected labels must all use this same value.
double planetOverviewCameraDistance(
    uint32_t viewportWidth,
    uint32_t viewportHeight,
    double verticalFovRadians,
    double requestedOrbitDistanceRadii);

struct PlanetNavigationSettings
{
  double ascentDurationSeconds = 2.0;
  double descentDurationSeconds = 2.0;
  double minimumOrbitDistanceRadii = 1.12;
  double maximumOrbitDistanceRadii = 6.0;
  // A little beyond two radii keeps relief and the atmosphere inside a 60
  // degree vertical field of view instead of clipping them at the poles.
  double defaultOrbitDistanceRadii = 2.2;
  // Matches the direct-manipulation speed of PolygonTerrainSample's globe.
  double orbitDragRadiansPerPixel = 0.00610865238;
  double orbitZoomExponentPerStep = 0.14;
  double clickDragThresholdPixels = 4.0;
  double overviewBlendStartRadii = 1.25;
  double overviewBlendEndRadii = 2.05;
  // The local camera follows the geodetic ascent only while local float
  // coordinates remain useful. Above this altitude the local view is held
  // still and faded out in favour of the normalized overview camera.
  double maximumLocalAltitudeMetres = 20000.0;
};

PlanetTangentFrame planetTangentFrame(const procedural::SpherePoint &direction);

PlanetNavigationPose interpolatePlanetNavigationPose(const PlanetNavigationPose &start, const PlanetNavigationPose &target, double progress);

class PlanetNavigationController
{
public:
  void initialize(const PlanetNavigationPose &surfacePose, const PlanetSurfaceCameraPose &surfaceCamera, double planetRadiusMetres);

  // M/button behavior. On the surface, currentSurfacePose and currentCamera
  // become the exact pose restored after the round trip. In every overview
  // state the same action starts the return transition.
  bool toggleOverview(const PlanetNavigationPose &currentSurfacePose, const PlanetSurfaceCameraPose &currentCamera, double nowSeconds);

  void update(double nowSeconds);

  bool beginLoadingTarget();
  bool cancelPendingTarget();
  bool completeLoadingTarget(
      const PlanetNavigationPose &targetSurfacePose,
      const PlanetSurfaceCameraPose &targetSurfaceCamera,
      double nowSeconds);

  PlanetNavigationState state() const
  {
    return state_;
  }
  const PlanetNavigationPose &pose() const
  {
    return pose_;
  }
  const PlanetSurfaceCameraPose &savedSurfaceCamera() const
  {
    return savedSurfaceCamera_;
  }
  double transitionProgress() const
  {
    return transitionProgress_;
  }

  bool surfaceInputEnabled() const
  {
    return state_ == PlanetNavigationState::Surface;
  }
  bool automaticTerrainStreamingEnabled() const
  {
    return state_ == PlanetNavigationState::Surface;
  }

  // Orbit pointer input becomes live during ascent as soon as the overview is
  // being revealed. Dragging updates both animation endpoints, preventing the
  // next transition tick from snapping the globe back.
  bool orbitInputEnabled() const;
  bool applyOrbitDrag(double deltaPixelsX, double deltaPixelsY);
  bool applyOrbitZoom(double wheelSteps);
  bool beginOrbitPointer(double cursorPixelsX, double cursorPixelsY);
  bool updateOrbitPointer(double cursorPixelsX, double cursorPixelsY);
  std::optional<std::array<double, 2>> endOrbitPointer(double cursorPixelsX, double cursorPixelsY);
  void cancelOrbitPointer();
  bool orbitPointerActive() const
  {
    return orbitPointerActive_;
  }
  bool orbitPointerDragging() const
  {
    return orbitPointerDragging_;
  }

  // The overview and local views overlap only through the bounded transition.
  // Orbit/LoadingTarget are overview-only; Surface is local-only.
  double overviewBlend() const;
  double localRenderWeight() const;
  bool voxelRenderingEnabled() const;
  bool voxelEffectsEnabled() const;

  // Stable normalized overview basis transported by orbit input. The fields
  // are radial camera direction, screen-right, and screen-up respectively.
  PlanetTangentFrame overviewCameraFrame() const;

  // Camera-centred tangent-space pose for the existing local renderer. It
  // never receives a planet-radius translation. During ascent its orientation
  // rotates toward the local nadir so the terrain/planet remains in view, and
  // descent reverses that animation to the exact saved surface orientation.
  PlanetSurfaceCameraPose localCameraPose() const;

  std::optional<PlanetSurfaceCameraPose> consumeSurfaceRestorePose();

  PlanetNavigationSettings &settings()
  {
    return settings_;
  }
  const PlanetNavigationSettings &settings() const
  {
    return settings_;
  }

private:
  void beginTransition(PlanetNavigationState state, const PlanetNavigationPose &target, const procedural::SpherePoint &targetCameraUp, double durationSeconds, double nowSeconds);

  PlanetNavigationState state_ = PlanetNavigationState::Surface;
  PlanetNavigationSettings settings_{};
  PlanetNavigationPose pose_{};
  PlanetNavigationPose surfacePose_{};
  PlanetNavigationPose orbitPose_{};
  PlanetNavigationPose transitionStartPose_{};
  PlanetNavigationPose transitionTargetPose_{};
  procedural::SpherePoint cameraUp_{0.0, 1.0, 0.0};
  procedural::SpherePoint surfaceCameraUp_{0.0, 1.0, 0.0};
  procedural::SpherePoint orbitCameraUp_{0.0, 1.0, 0.0};
  procedural::SpherePoint transitionStartCameraUp_{0.0, 1.0, 0.0};
  procedural::SpherePoint transitionTargetCameraUp_{0.0, 1.0, 0.0};
  PlanetSurfaceCameraPose savedSurfaceCamera_{};
  double planetRadiusMetres_ = 1.0;
  double transitionStartSeconds_ = 0.0;
  double transitionDurationSeconds_ = 0.0;
  double transitionProgress_ = 0.0;
  bool surfaceRestorePending_ = false;
  bool orbitPointerActive_ = false;
  bool orbitPointerDragging_ = false;
  std::array<double, 2> orbitPointerStart_{};
  std::array<double, 2> orbitPointerLast_{};
};

} // namespace voxel::svraster
