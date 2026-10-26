#include "voxel/svraster/PlanetNavigation.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace
{

int failures = 0;

void check(const bool condition, const char *message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

double dot(const procedural::SpherePoint &a, const procedural::SpherePoint &b)
{
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

double length(const procedural::SpherePoint &value)
{
  return std::sqrt(dot(value, value));
}

procedural::SpherePoint rotate(
    voxel::svraster::PlanetNavigationQuaternion orientation,
    const procedural::SpherePoint &value)
{
  const double inverseLength = 1.0 / std::sqrt(
      orientation.x * orientation.x + orientation.y * orientation.y +
      orientation.z * orientation.z + orientation.w * orientation.w);
  orientation.x *= inverseLength;
  orientation.y *= inverseLength;
  orientation.z *= inverseLength;
  orientation.w *= inverseLength;
  const procedural::SpherePoint axis{
      orientation.x, orientation.y, orientation.z};
  const auto cross = [](const procedural::SpherePoint &a,
                         const procedural::SpherePoint &b)
  {
    return procedural::SpherePoint{
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x};
  };
  const procedural::SpherePoint first = cross(axis, value);
  const procedural::SpherePoint second = cross(axis, first);
  return {
      value.x + 2.0 * (orientation.w * first.x + second.x),
      value.y + 2.0 * (orientation.w * first.y + second.y),
      value.z + 2.0 * (orientation.w * first.z + second.z)};
}

bool near(const double a, const double b, const double epsilon = 1.0e-10)
{
  return std::abs(a - b) <= epsilon;
}

voxel::svraster::PlanetNavigationPose surfacePose()
{
  voxel::svraster::PlanetNavigationPose pose{};
  pose.direction = {0.2, 0.9, -0.3};
  pose.altitudeMetres = 1234.5;
  pose.orbitDistanceRadii = 1.0;
  pose.orientation = {0.0, std::sin(0.25), 0.0, std::cos(0.25)};
  return pose;
}

voxel::svraster::PlanetSurfaceCameraPose cameraPose()
{
  return {{12.0, 34.0, -56.0}, {0.1, 0.2, 0.3, 0.9}};
}

} // namespace

int main()
{
  namespace svr = voxel::svraster;
  std::cout << "sparse voxel planet navigation\n";

  for (const procedural::SpherePoint direction : {procedural::SpherePoint{0.3, 0.8, -0.5}, procedural::SpherePoint{0.0, 1.0, 0.0}, procedural::SpherePoint{0.0, -1.0, 0.0}})
  {
    const svr::PlanetTangentFrame frame = svr::planetTangentFrame(direction);
    check(
        near(length(frame.up), 1.0) && near(length(frame.east), 1.0) && near(length(frame.north), 1.0) && near(dot(frame.up, frame.east), 0.0) && near(dot(frame.up, frame.north), 0.0) &&
            near(dot(frame.east, frame.north), 0.0),
        "tangent frame is orthonormal at ordinary directions and poles");
  }

  const svr::PlanetNavigationPose start = surfacePose();
  svr::PlanetNavigationPose target = start;
  target.direction = {-0.4, 0.1, 0.9};
  target.altitudeMetres = 6371000.0;
  target.orbitDistanceRadii = 2.0;
  target.orientation = {0.0, 1.0, 0.0, 0.0};
  const svr::PlanetNavigationPose endpoint0 = svr::interpolatePlanetNavigationPose(start, target, 0.0);
  const svr::PlanetNavigationPose endpoint1 = svr::interpolatePlanetNavigationPose(start, target, 1.0);
  const svr::PlanetNavigationPose midpoint = svr::interpolatePlanetNavigationPose(start, target, 0.5);
  check(
      near(endpoint0.altitudeMetres, start.altitudeMetres) && near(endpoint1.altitudeMetres, target.altitudeMetres) && near(endpoint0.direction.x, start.direction.x / length(start.direction)) &&
          near(endpoint1.direction.z, target.direction.z / length(target.direction)),
      "animation interpolation reaches exact normalized endpoints");
  check(
      midpoint.altitudeMetres > start.altitudeMetres && midpoint.altitudeMetres < target.altitudeMetres && near(length(midpoint.direction), 1.0) &&
          near(
              std::sqrt(
                  midpoint.orientation.x * midpoint.orientation.x + midpoint.orientation.y * midpoint.orientation.y + midpoint.orientation.z * midpoint.orientation.z + midpoint.orientation.w * midpoint.orientation.w),
              1.0),
      "animation keeps direction and orientation normalized");

  svr::PlanetNavigationController controller;
  controller.initialize(start, cameraPose(), 6371000.0);
  check(controller.state() == svr::PlanetNavigationState::Surface && controller.automaticTerrainStreamingEnabled(), "navigation initializes on the streaming surface");
  check(
      controller.toggleOverview(start, cameraPose(), 10.0) && controller.state() == svr::PlanetNavigationState::Ascending && !controller.automaticTerrainStreamingEnabled(),
      "M starts ascent and immediately gates automatic terrain streaming");
  controller.update(11.0);
  check(controller.state() == svr::PlanetNavigationState::Ascending && controller.transitionProgress() > 0.0 && controller.transitionProgress() < 1.0, "ascent advances without blocking");
  controller.update(12.0);
  check(controller.state() == svr::PlanetNavigationState::Orbit && near(controller.pose().orbitDistanceRadii, 2.2), "ascent reaches a fully framed orbit at the configured endpoint");
  check(
      controller.beginLoadingTarget() && controller.state() == svr::PlanetNavigationState::LoadingTarget && controller.cancelPendingTarget() && controller.state() == svr::PlanetNavigationState::Orbit,
      "Escape-style cancellation returns a pending target to orbit");
  controller.toggleOverview(start, cameraPose(), 13.0);
  controller.update(15.0);
  const auto restored = controller.consumeSurfaceRestorePose();
  check(
      controller.state() == svr::PlanetNavigationState::Surface && restored && restored->positionMetres == cameraPose().positionMetres &&
          near(restored->orientation.x, cameraPose().orientation.x) && near(restored->orientation.y, cameraPose().orientation.y) && near(restored->orientation.z, cameraPose().orientation.z) &&
          near(restored->orientation.w, cameraPose().orientation.w) && near(controller.pose().altitudeMetres, start.altitudeMetres),
      "descent restores the exact saved local camera and geodetic pose");

  controller.toggleOverview(start, cameraPose(), 20.0);
  controller.update(20.5);
  const procedural::SpherePoint ascentDirectionBeforeDrag =
      controller.pose().direction;
  check(
      controller.orbitInputEnabled() &&
          controller.beginOrbitPointer(100.0, 100.0) &&
          controller.updateOrbitPointer(180.0, 135.0) &&
          controller.orbitPointerDragging() &&
          dot(ascentDirectionBeforeDrag, controller.pose().direction) < 0.999,
      "overview drag rotates the globe during ascent");
  const procedural::SpherePoint ascentDirectionAfterDrag =
      controller.pose().direction;
  controller.update(20.6);
  check(
      dot(ascentDirectionAfterDrag, controller.pose().direction) > 0.999999,
      "ascent animation preserves the dragged globe orientation");
  controller.cancelOrbitPointer();
  controller.toggleOverview(start, cameraPose(), 20.5);
  check(controller.state() == svr::PlanetNavigationState::Descending, "repeated M reverses an in-progress ascent");
  controller.toggleOverview(start, cameraPose(), 20.75);
  check(controller.state() == svr::PlanetNavigationState::Ascending, "repeated M reverses an in-progress descent");

  svr::PlanetNavigationController orbitController;
  orbitController.initialize(start, cameraPose(), 6371000.0);
  check(!orbitController.applyOrbitDrag(20.0, 10.0) && !orbitController.applyOrbitZoom(1.0) && near(orbitController.overviewBlend(), 0.0), "surface mode rejects orbit controls and remains local-only");
  orbitController.toggleOverview(start, cameraPose(), 30.0);
  orbitController.update(31.0);
  const double midpointBlend = orbitController.overviewBlend();
  const auto midpointCamera = orbitController.localCameraPose();
  const procedural::SpherePoint surfaceForward =
      rotate(cameraPose().orientation, {0.0, 0.0, -1.0});
  const procedural::SpherePoint midpointForward =
      rotate(midpointCamera.orientation, {0.0, 0.0, -1.0});
  check(
      midpointBlend > 0.0 && midpointBlend < 1.0 && midpointCamera.positionMetres[1] > cameraPose().positionMetres[1] &&
          midpointCamera.positionMetres[1] <= cameraPose().positionMetres[1] + orbitController.settings().maximumLocalAltitudeMetres &&
          midpointForward.y < surfaceForward.y,
      "ascent derives bounded local altitude, rotates toward the planet, and continuously blends the overview");
  orbitController.update(32.0);
  const procedural::SpherePoint orbitForward = rotate(
      orbitController.localCameraPose().orientation, {0.0, 0.0, -1.0});
  check(
      orbitForward.y < -0.995,
      "orbit endpoint keeps the local camera pointed toward the planet center");
  const procedural::SpherePoint directionBeforeDrag = orbitController.pose().direction;
  const double distanceBeforeZoom = orbitController.pose().orbitDistanceRadii;
  check(
      orbitController.orbitInputEnabled() && orbitController.applyOrbitDrag(120.0, -75.0) && dot(directionBeforeDrag, orbitController.pose().direction) < 0.999 && orbitController.applyOrbitZoom(1.0) &&
          orbitController.pose().orbitDistanceRadii < distanceBeforeZoom,
      "orbit drag rotates the geodetic camera and wheel zoom is exponential");
  const svr::PlanetTangentFrame orbitFrame = orbitController.overviewCameraFrame();
  check(
      near(length(orbitFrame.up), 1.0) && near(length(orbitFrame.east), 1.0) && near(length(orbitFrame.north), 1.0) && near(dot(orbitFrame.up, orbitFrame.east), 0.0) && near(dot(orbitFrame.up, orbitFrame.north), 0.0) &&
          near(dot(orbitFrame.east, orbitFrame.north), 0.0),
      "transported orbit basis remains orthonormal after two-axis drag");
  orbitController.applyOrbitZoom(1000.0);
  check(
      near(orbitController.pose().orbitDistanceRadii, orbitController.settings().minimumOrbitDistanceRadii) && near(orbitController.overviewBlend(), 1.0) && !orbitController.voxelRenderingEnabled() &&
          !orbitController.voxelEffectsEnabled(),
      "overview mode clamps close zoom and bypasses voxel rendering effects");
  orbitController.applyOrbitZoom(-1000.0);
  check(near(orbitController.pose().orbitDistanceRadii, orbitController.settings().maximumOrbitDistanceRadii), "overview mode clamps distant zoom without losing precision");

  bool poleCrossingStable = true;
  for (uint32_t step = 0u; step < 24u; ++step)
  {
    orbitController.applyOrbitDrag(17.0, 95.0);
    const svr::PlanetTangentFrame frame = orbitController.overviewCameraFrame();
    poleCrossingStable &= near(length(frame.up), 1.0) && near(length(frame.east), 1.0) && near(length(frame.north), 1.0) && near(dot(frame.up, frame.east), 0.0) && near(dot(frame.up, frame.north), 0.0) &&
                          near(dot(frame.east, frame.north), 0.0);
  }
  check(poleCrossingStable, "transported orbit orientation crosses poles without a singularity");

  std::array<double, 2> projected{};
  const svr::PlanetTangentFrame projectionFrame =
      svr::planetTangentFrame({0.0, 0.0, 1.0});
  const double landscapeFit = svr::planetOverviewCameraDistance(
      1280u, 720u, 1.0471975511965976, 2.2);
  const double portraitFit = svr::planetOverviewCameraDistance(
      720u, 1280u, 1.0471975511965976, 2.2);
  check(
      landscapeFit > 2.2 && portraitFit > landscapeFit,
      "overview fit distance frames the complete globe in landscape and portrait viewports");
  check(
      svr::planetOverviewProjectDirection(
          {0.0, 0.0, 1.0}, 1280u, 720u,
          1.0471975511965976, projectionFrame, landscapeFit, projected) &&
          near(projected[0], 640.0) && near(projected[1], 360.0),
      "a visible destination projects to the overview screen centre");
  check(
      !svr::planetOverviewProjectDirection(
          {0.0, 0.0, -1.0}, 1280u, 720u,
          1.0471975511965976, projectionFrame, 2.2, projected),
      "a hidden-hemisphere destination does not produce a floating label");

  svr::PlanetNavigationRequestGate requestGate;
  std::array<uint64_t, 8> rapidRequests{};
  for (uint64_t &request : rapidRequests)
    request = requestGate.submit();
  check(
      std::none_of(
          rapidRequests.begin(), rapidRequests.end() - 1,
          [&requestGate](const uint64_t request)
          {
            return requestGate.accepts(request);
          }) &&
          requestGate.accepts(rapidRequests.back()),
      "rapid destination changes leave only the final generation publishable");
  const uint64_t cancelledGeneration = requestGate.cancel();
  check(
      !requestGate.accepts(rapidRequests.back()) &&
          requestGate.accepts(cancelledGeneration),
      "cancelling advances the publication gate");

  svr::PlanetNavigationController targetController;
  targetController.initialize(start, cameraPose(), 6371000.0);
  targetController.toggleOverview(start, cameraPose(), 50.0);
  targetController.update(52.0);
  svr::PlanetNavigationPose landingPose = start;
  landingPose.direction = {-0.8, 0.3, 0.2};
  landingPose.altitudeMetres = 845.0;
  svr::PlanetSurfaceCameraPose landingCamera = cameraPose();
  landingCamera.positionMetres = {0.0, 7.0, 0.0};
  check(
      targetController.beginLoadingTarget() &&
          targetController.completeLoadingTarget(
              landingPose, landingCamera, 52.0) &&
          targetController.state() == svr::PlanetNavigationState::Descending,
      "a published target starts descent only from loading state");
  targetController.update(54.0);
  const auto targetRestore = targetController.consumeSurfaceRestorePose();
  check(
      targetController.state() == svr::PlanetNavigationState::Surface &&
          targetRestore &&
          targetRestore->positionMetres == landingCamera.positionMetres &&
          near(targetController.pose().altitudeMetres,
               landingPose.altitudeMetres),
      "target descent lands on the newly published local camera pose");

  svr::PlanetNavigationController descentController;
  descentController.initialize(start, cameraPose(), 6371000.0);
  descentController.toggleOverview(start, cameraPose(), 40.0);
  descentController.update(42.0);
  descentController.toggleOverview(start, cameraPose(), 42.0);
  descentController.update(43.0);
  check(
      descentController.state() == svr::PlanetNavigationState::Descending && descentController.overviewBlend() > 0.0 && descentController.overviewBlend() < 1.0 && descentController.localRenderWeight() > 0.0 &&
          descentController.localRenderWeight() < 1.0,
      "descent continuously fades the overview back to the local renderer");

  return failures == 0 ? 0 : 1;
}
