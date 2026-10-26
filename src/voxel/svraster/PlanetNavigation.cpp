#include "voxel/svraster/PlanetNavigation.hpp"

#include <algorithm>
#include <cmath>

namespace voxel::svraster
{
namespace
{

constexpr double kDirectionEpsilon = 1.0e-24;
// Camera::updateMatrices uses a fixed world-up vector, so an exactly vertical
// forward vector makes its look-at basis singular. This is visually nadir but
// retains a small, stable horizontal component for the local terrain view.
constexpr double kPlanetCenterLookPitchRadians = 1.53;
constexpr double kOverviewFramingRadius = 1.08;
// Keep a slim safety margin for labels and projection rounding, but let the
// globe use substantially more of the limiting viewport dimension.
constexpr double kOverviewViewportFill = 0.90;

double dot(const procedural::SpherePoint &a, const procedural::SpherePoint &b)
{
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

procedural::SpherePoint cross(const procedural::SpherePoint &a, const procedural::SpherePoint &b)
{
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

procedural::SpherePoint normalized(procedural::SpherePoint value)
{
  const double squaredLength = dot(value, value);
  if (!(squaredLength > kDirectionEpsilon) || !std::isfinite(squaredLength))
    return {0.0, 0.0, 1.0};
  const double inverseLength = 1.0 / std::sqrt(squaredLength);
  value.x *= inverseLength;
  value.y *= inverseLength;
  value.z *= inverseLength;
  return value;
}

procedural::SpherePoint scaled(const procedural::SpherePoint &value, const double amount)
{
  return {value.x * amount, value.y * amount, value.z * amount};
}

procedural::SpherePoint added(const procedural::SpherePoint &a, const procedural::SpherePoint &b)
{
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}

procedural::SpherePoint rotateAroundAxis(const procedural::SpherePoint &value, const procedural::SpherePoint &axis, const double angle)
{
  const procedural::SpherePoint unitAxis = normalized(axis);
  const double cosine = std::cos(angle);
  const double sine = std::sin(angle);
  return added(added(scaled(value, cosine), scaled(cross(unitAxis, value), sine)), scaled(unitAxis, dot(unitAxis, value) * (1.0 - cosine)));
}

procedural::SpherePoint cameraUpForDirection(const procedural::SpherePoint &direction, const procedural::SpherePoint &candidate)
{
  const procedural::SpherePoint radial = normalized(direction);
  procedural::SpherePoint tangent = added(candidate, scaled(radial, -dot(candidate, radial)));
  if (dot(tangent, tangent) <= kDirectionEpsilon)
    tangent = planetTangentFrame(radial).north;
  return normalized(tangent);
}

procedural::SpherePoint slerpDirection(const procedural::SpherePoint &from, const procedural::SpherePoint &to, const double progress)
{
  if (progress <= 0.0)
    return normalized(from);
  if (progress >= 1.0)
    return normalized(to);
  const procedural::SpherePoint a = normalized(from);
  const procedural::SpherePoint b = normalized(to);
  const double cosine = std::clamp(dot(a, b), -1.0, 1.0);
  if (cosine > 1.0 - 1.0e-10)
    return normalized({a.x + (b.x - a.x) * progress, a.y + (b.y - a.y) * progress, a.z + (b.z - a.z) * progress});

  // There is no unique shortest arc for antipodal points. Choose a stable
  // orthogonal great-circle axis so the result remains deterministic.
  if (cosine < -1.0 + 1.0e-10)
  {
    const procedural::SpherePoint reference = std::abs(a.y) < 0.9 ? procedural::SpherePoint{0.0, 1.0, 0.0} : procedural::SpherePoint{1.0, 0.0, 0.0};
    const procedural::SpherePoint orthogonal = normalized(cross(reference, a));
    const double angle = std::acos(-1.0) * progress;
    return normalized({a.x * std::cos(angle) + orthogonal.x * std::sin(angle), a.y * std::cos(angle) + orthogonal.y * std::sin(angle), a.z * std::cos(angle) + orthogonal.z * std::sin(angle)});
  }

  const double angle = std::acos(cosine);
  const double inverseSine = 1.0 / std::sin(angle);
  const double fromWeight = std::sin((1.0 - progress) * angle) * inverseSine;
  const double toWeight = std::sin(progress * angle) * inverseSine;
  return normalized({a.x * fromWeight + b.x * toWeight, a.y * fromWeight + b.y * toWeight, a.z * fromWeight + b.z * toWeight});
}

PlanetNavigationQuaternion normalized(PlanetNavigationQuaternion value)
{
  const double squaredLength = value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w;
  if (!(squaredLength > kDirectionEpsilon) || !std::isfinite(squaredLength))
    return {};
  const double inverseLength = 1.0 / std::sqrt(squaredLength);
  value.x *= inverseLength;
  value.y *= inverseLength;
  value.z *= inverseLength;
  value.w *= inverseLength;
  return value;
}

PlanetNavigationQuaternion multiplied(
    const PlanetNavigationQuaternion &a,
    const PlanetNavigationQuaternion &b)
{
  return normalized({
      a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
      a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
      a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
      a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z});
}

procedural::SpherePoint rotated(
    const PlanetNavigationQuaternion &orientation,
    const procedural::SpherePoint &value)
{
  const PlanetNavigationQuaternion unit = normalized(orientation);
  const procedural::SpherePoint axis{unit.x, unit.y, unit.z};
  const procedural::SpherePoint first = cross(axis, value);
  const procedural::SpherePoint second = cross(axis, first);
  return added(value, scaled(added(scaled(first, unit.w), second), 2.0));
}

PlanetNavigationQuaternion rotationBetween(
    const procedural::SpherePoint &from,
    const procedural::SpherePoint &to)
{
  const procedural::SpherePoint a = normalized(from);
  const procedural::SpherePoint b = normalized(to);
  const double cosine = std::clamp(dot(a, b), -1.0, 1.0);
  if (cosine < -1.0 + 1.0e-10)
  {
    const procedural::SpherePoint reference =
        std::abs(a.y) < 0.9 ? procedural::SpherePoint{0.0, 1.0, 0.0}
                            : procedural::SpherePoint{1.0, 0.0, 0.0};
    const procedural::SpherePoint axis = normalized(cross(a, reference));
    return {axis.x, axis.y, axis.z, 0.0};
  }
  const procedural::SpherePoint axis = cross(a, b);
  return normalized({axis.x, axis.y, axis.z, 1.0 + cosine});
}

PlanetNavigationQuaternion planetCenterLookingOrientation(
    const PlanetNavigationQuaternion &surfaceOrientation)
{
  const PlanetNavigationQuaternion surface = normalized(surfaceOrientation);
  const procedural::SpherePoint forward =
      normalized(rotated(surface, {0.0, 0.0, -1.0}));
  procedural::SpherePoint horizontal{forward.x, 0.0, forward.z};
  if (dot(horizontal, horizontal) <= kDirectionEpsilon)
  {
    const procedural::SpherePoint cameraUp =
        rotated(surface, {0.0, 1.0, 0.0});
    horizontal = {cameraUp.x, 0.0, cameraUp.z};
  }
  if (dot(horizontal, horizontal) <= kDirectionEpsilon)
    horizontal = {0.0, 0.0, -1.0};
  horizontal = normalized(horizontal);
  const procedural::SpherePoint centerFacing = normalized(procedural::SpherePoint{
      horizontal.x * std::cos(kPlanetCenterLookPitchRadians),
      -std::sin(kPlanetCenterLookPitchRadians),
      horizontal.z * std::cos(kPlanetCenterLookPitchRadians)});
  return multiplied(rotationBetween(forward, centerFacing), surface);
}

PlanetNavigationQuaternion slerpQuaternion(const PlanetNavigationQuaternion &from, const PlanetNavigationQuaternion &to, const double progress)
{
  if (progress <= 0.0)
    return normalized(from);
  if (progress >= 1.0)
    return normalized(to);
  const PlanetNavigationQuaternion a = normalized(from);
  PlanetNavigationQuaternion b = normalized(to);
  double cosine = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
  if (cosine < 0.0)
  {
    cosine = -cosine;
    b = {-b.x, -b.y, -b.z, -b.w};
  }
  if (cosine > 1.0 - 1.0e-10)
    return normalized({a.x + (b.x - a.x) * progress, a.y + (b.y - a.y) * progress, a.z + (b.z - a.z) * progress, a.w + (b.w - a.w) * progress});
  const double angle = std::acos(std::clamp(cosine, -1.0, 1.0));
  const double inverseSine = 1.0 / std::sin(angle);
  const double fromWeight = std::sin((1.0 - progress) * angle) * inverseSine;
  const double toWeight = std::sin(progress * angle) * inverseSine;
  return normalized({a.x * fromWeight + b.x * toWeight, a.y * fromWeight + b.y * toWeight, a.z * fromWeight + b.z * toWeight, a.w * fromWeight + b.w * toWeight});
}

double interpolateAltitude(const double start, const double target, const double progress)
{
  if (progress <= 0.0)
    return start;
  if (progress >= 1.0)
    return target;
  if (start >= 0.0 && target >= 0.0)
    return std::expm1(std::log1p(start) + (std::log1p(target) - std::log1p(start)) * progress);
  return start + (target - start) * progress;
}

PlanetNavigationPose sanitizedPose(PlanetNavigationPose pose)
{
  pose.direction = normalized(pose.direction);
  pose.orientation = normalized(pose.orientation);
  if (!std::isfinite(pose.altitudeMetres))
    pose.altitudeMetres = 0.0;
  if (!std::isfinite(pose.orbitDistanceRadii))
    pose.orbitDistanceRadii = 1.12;
  return pose;
}

} // namespace

const char *planetNavigationStateName(const PlanetNavigationState state)
{
  switch (state)
  {
  case PlanetNavigationState::Surface:
    return "Surface";
  case PlanetNavigationState::Ascending:
    return "Ascending";
  case PlanetNavigationState::Orbit:
    return "Orbit";
  case PlanetNavigationState::LoadingTarget:
    return "Loading target";
  case PlanetNavigationState::Descending:
    return "Descending";
  }
  return "Unknown";
}

uint64_t PlanetNavigationRequestGate::advance()
{
  ++generation_;
  if (generation_ == 0u)
    ++generation_;
  return generation_;
}

uint64_t PlanetNavigationRequestGate::submit()
{
  return advance();
}

uint64_t PlanetNavigationRequestGate::cancel()
{
  return advance();
}

bool PlanetNavigationRequestGate::accepts(const uint64_t generation) const
{
  return generation != 0u && generation == generation_;
}

PlanetTangentFrame planetTangentFrame(const procedural::SpherePoint &direction)
{
  PlanetTangentFrame result{};
  result.up = normalized(direction);
  const procedural::SpherePoint reference = std::abs(result.up.y) < 0.9 ? procedural::SpherePoint{0.0, 1.0, 0.0} : procedural::SpherePoint{1.0, 0.0, 0.0};
  result.east = normalized(cross(reference, result.up));
  result.north = normalized(cross(result.up, result.east));
  return result;
}

bool planetOverviewCursorRay(
    const double cursorPixelsX,
    const double cursorPixelsY,
    const uint32_t viewportWidth,
    const uint32_t viewportHeight,
    const double verticalFovRadians,
    const PlanetTangentFrame &cameraFrame,
    const double orbitDistanceRadii,
    PlanetOverviewRay &ray)
{
  ray = {};
  if (viewportWidth == 0u || viewportHeight == 0u || !std::isfinite(cursorPixelsX) || !std::isfinite(cursorPixelsY) || !std::isfinite(verticalFovRadians) || verticalFovRadians <= 0.0 ||
      verticalFovRadians >= std::acos(-1.0) || !std::isfinite(orbitDistanceRadii) || orbitDistanceRadii <= 1.0)
    return false;
  const procedural::SpherePoint radial = normalized(cameraFrame.up);
  const procedural::SpherePoint screenUp = cameraUpForDirection(radial, cameraFrame.north);
  const procedural::SpherePoint screenRight = normalized(cross(screenUp, radial));
  const double aspect = static_cast<double>(viewportWidth) / static_cast<double>(viewportHeight);
  const double halfHeight = std::tan(verticalFovRadians * 0.5);
  const double ndcX = 2.0 * cursorPixelsX / viewportWidth - 1.0;
  // Vulkan's positive-height viewport maps NDC -1 to the top row and +1 to
  // the bottom row. The overview vertex shader follows that convention.
  const double ndcY = 2.0 * cursorPixelsY / viewportHeight - 1.0;
  ray.origin = scaled(radial, orbitDistanceRadii);
  // The overview vertex shader stores camera-up with a negated view Y. Keep
  // cursor unprojection exactly inverse to that convention.
  ray.direction = normalized(added(added(scaled(radial, -1.0), scaled(screenRight, ndcX * aspect * halfHeight)), scaled(screenUp, -ndcY * halfHeight)));
  return true;
}

bool planetRaySphereIntersection(const PlanetOverviewRay &ray, const double sphereRadius, double &distance, procedural::SpherePoint *position)
{
  distance = 0.0;
  if (!std::isfinite(sphereRadius) || sphereRadius <= 0.0)
    return false;
  const procedural::SpherePoint direction = normalized(ray.direction);
  const double projection = dot(ray.origin, direction);
  const double constant = dot(ray.origin, ray.origin) - sphereRadius * sphereRadius;
  const double discriminant = projection * projection - constant;
  if (discriminant < 0.0 || !std::isfinite(discriminant))
    return false;
  const double root = std::sqrt(discriminant);
  double hitDistance = -projection - root;
  if (hitDistance < 0.0)
    hitDistance = -projection + root;
  if (hitDistance < 0.0 || !std::isfinite(hitDistance))
    return false;
  distance = hitDistance;
  if (position != nullptr)
    *position = added(ray.origin, scaled(direction, hitDistance));
  return true;
}

bool planetOverviewPickDirection(
    const double cursorPixelsX,
    const double cursorPixelsY,
    const uint32_t viewportWidth,
    const uint32_t viewportHeight,
    const double verticalFovRadians,
    const PlanetTangentFrame &cameraFrame,
    const double orbitDistanceRadii,
    procedural::SpherePoint &direction)
{
  PlanetOverviewRay ray{};
  procedural::SpherePoint hit{};
  double hitDistance = 0.0;
  if (!planetOverviewCursorRay(cursorPixelsX, cursorPixelsY, viewportWidth, viewportHeight, verticalFovRadians, cameraFrame, orbitDistanceRadii, ray) || !planetRaySphereIntersection(ray, 1.0, hitDistance, &hit))
    return false;
  direction = normalized(hit);
  return true;
}

bool planetOverviewProjectDirection(
    const procedural::SpherePoint &direction,
    const uint32_t viewportWidth,
    const uint32_t viewportHeight,
    const double verticalFovRadians,
    const PlanetTangentFrame &cameraFrame,
    const double orbitDistanceRadii,
    std::array<double, 2> &cursorPixels)
{
  cursorPixels = {};
  if (viewportWidth == 0u || viewportHeight == 0u ||
      !std::isfinite(verticalFovRadians) || verticalFovRadians <= 0.0 ||
      verticalFovRadians >= std::acos(-1.0) ||
      !std::isfinite(orbitDistanceRadii) || orbitDistanceRadii <= 1.0)
    return false;
  const procedural::SpherePoint point = normalized(direction);
  const procedural::SpherePoint radial = normalized(cameraFrame.up);
  // A surface marker is visible only when its outward normal faces the orbit
  // camera. This matches the globe's back-face/depth visibility boundary.
  if (dot(point, radial) * orbitDistanceRadii <= 1.0)
    return false;
  const procedural::SpherePoint screenUp =
      cameraUpForDirection(radial, cameraFrame.north);
  const procedural::SpherePoint screenRight = normalized(cross(screenUp, radial));
  const procedural::SpherePoint camera = scaled(radial, orbitDistanceRadii);
  const procedural::SpherePoint relative = added(point, scaled(camera, -1.0));
  const double viewX = dot(relative, screenRight);
  const double viewY = -dot(relative, screenUp);
  const double viewZ = dot(relative, scaled(radial, -1.0));
  if (!(viewZ > 0.0) || !std::isfinite(viewZ))
    return false;
  const double focal = 1.0 / std::tan(verticalFovRadians * 0.5);
  const double aspect =
      static_cast<double>(viewportWidth) / viewportHeight;
  const double ndcX = viewX * focal / (aspect * viewZ);
  const double ndcY = viewY * focal / viewZ;
  if (!std::isfinite(ndcX) || !std::isfinite(ndcY) ||
      ndcX < -1.0 || ndcX > 1.0 || ndcY < -1.0 || ndcY > 1.0)
    return false;
  cursorPixels = {
    (ndcX + 1.0) * 0.5 * viewportWidth,
    (ndcY + 1.0) * 0.5 * viewportHeight,
  };
  return true;
}

double planetOverviewCameraDistance(
    const uint32_t viewportWidth,
    const uint32_t viewportHeight,
    const double verticalFovRadians,
    const double requestedOrbitDistanceRadii)
{
  const double requested =
      std::isfinite(requestedOrbitDistanceRadii) &&
              requestedOrbitDistanceRadii > 1.0
          ? requestedOrbitDistanceRadii
          : 1.000001;
  if (viewportWidth == 0u || viewportHeight == 0u ||
      !std::isfinite(verticalFovRadians) || verticalFovRadians <= 0.0 ||
      verticalFovRadians >= std::acos(-1.0))
    return requested;
  const double verticalHalfFov = verticalFovRadians * 0.5;
  const double aspect =
      static_cast<double>(viewportWidth) / viewportHeight;
  const double horizontalHalfFov =
      std::atan(std::tan(verticalHalfFov) * aspect);
  const double limitingHalfFov =
      std::min(verticalHalfFov, horizontalHalfFov);
  const double framedHalfAngle = std::atan(
      std::tan(limitingHalfFov) * kOverviewViewportFill);
  const double fitted =
      kOverviewFramingRadius / std::sin(framedHalfAngle);
  return std::max(requested, fitted);
}

PlanetNavigationPose interpolatePlanetNavigationPose(const PlanetNavigationPose &start, const PlanetNavigationPose &target, const double progress)
{
  if (progress <= 0.0)
    return sanitizedPose(start);
  if (progress >= 1.0)
    return sanitizedPose(target);
  const double t = std::clamp(progress, 0.0, 1.0);
  const double eased = t * t * (3.0 - 2.0 * t);
  PlanetNavigationPose result{};
  result.direction = slerpDirection(start.direction, target.direction, eased);
  result.altitudeMetres = interpolateAltitude(start.altitudeMetres, target.altitudeMetres, eased);
  result.orbitDistanceRadii = start.orbitDistanceRadii + (target.orbitDistanceRadii - start.orbitDistanceRadii) * eased;
  result.orientation = slerpQuaternion(start.orientation, target.orientation, eased);
  return result;
}

void PlanetNavigationController::initialize(const PlanetNavigationPose &surfacePose, const PlanetSurfaceCameraPose &surfaceCamera, const double planetRadiusMetres)
{
  planetRadiusMetres_ = std::isfinite(planetRadiusMetres) && planetRadiusMetres > 0.0 ? planetRadiusMetres : 1.0;
  surfacePose_ = sanitizedPose(surfacePose);
  savedSurfaceCamera_ = surfaceCamera;
  pose_ = surfacePose_;
  orbitPose_ = surfacePose_;
  orbitPose_.orbitDistanceRadii = std::clamp(settings_.defaultOrbitDistanceRadii, settings_.minimumOrbitDistanceRadii, settings_.maximumOrbitDistanceRadii);
  orbitPose_.altitudeMetres = planetRadiusMetres_ * (orbitPose_.orbitDistanceRadii - 1.0);
  orbitPose_.orientation =
      planetCenterLookingOrientation(surfacePose_.orientation);
  surfaceCameraUp_ = planetTangentFrame(surfacePose_.direction).north;
  orbitCameraUp_ = surfaceCameraUp_;
  cameraUp_ = surfaceCameraUp_;
  transitionStartPose_ = pose_;
  transitionTargetPose_ = pose_;
  transitionStartCameraUp_ = cameraUp_;
  transitionTargetCameraUp_ = cameraUp_;
  transitionStartSeconds_ = 0.0;
  transitionDurationSeconds_ = 0.0;
  transitionProgress_ = 0.0;
  surfaceRestorePending_ = false;
  cancelOrbitPointer();
  state_ = PlanetNavigationState::Surface;
}

void PlanetNavigationController::beginTransition(
    const PlanetNavigationState state,
    const PlanetNavigationPose &target,
    const procedural::SpherePoint &targetCameraUp,
    const double durationSeconds,
    const double nowSeconds)
{
  cancelOrbitPointer();
  state_ = state;
  transitionStartPose_ = pose_;
  transitionTargetPose_ = sanitizedPose(target);
  transitionStartCameraUp_ = cameraUp_;
  transitionTargetCameraUp_ = cameraUpForDirection(transitionTargetPose_.direction, targetCameraUp);
  transitionStartSeconds_ = std::isfinite(nowSeconds) ? nowSeconds : 0.0;
  transitionDurationSeconds_ = std::isfinite(durationSeconds) ? std::max(durationSeconds, 0.0) : 0.0;
  transitionProgress_ = 0.0;
  surfaceRestorePending_ = false;
  if (transitionDurationSeconds_ == 0.0)
    update(transitionStartSeconds_);
}

bool PlanetNavigationController::toggleOverview(const PlanetNavigationPose &currentSurfacePose, const PlanetSurfaceCameraPose &currentCamera, const double nowSeconds)
{
  update(nowSeconds);
  if (state_ == PlanetNavigationState::Surface)
  {
    surfacePose_ = sanitizedPose(currentSurfacePose);
    savedSurfaceCamera_ = currentCamera;
    pose_ = surfacePose_;
    orbitPose_ = surfacePose_;
    orbitPose_.orbitDistanceRadii = std::clamp(settings_.defaultOrbitDistanceRadii, settings_.minimumOrbitDistanceRadii, settings_.maximumOrbitDistanceRadii);
    orbitPose_.altitudeMetres = planetRadiusMetres_ * (orbitPose_.orbitDistanceRadii - 1.0);
    orbitPose_.orientation =
        planetCenterLookingOrientation(surfacePose_.orientation);
    surfaceCameraUp_ = cameraUpForDirection(surfacePose_.direction, surfaceCameraUp_);
    orbitCameraUp_ = surfaceCameraUp_;
    cameraUp_ = surfaceCameraUp_;
    beginTransition(PlanetNavigationState::Ascending, orbitPose_, orbitCameraUp_, settings_.ascentDurationSeconds, nowSeconds);
    return true;
  }

  if (state_ == PlanetNavigationState::Descending)
  {
    beginTransition(PlanetNavigationState::Ascending, orbitPose_, orbitCameraUp_, settings_.ascentDurationSeconds, nowSeconds);
    return true;
  }

  beginTransition(PlanetNavigationState::Descending, surfacePose_, surfaceCameraUp_, settings_.descentDurationSeconds, nowSeconds);
  return true;
}

void PlanetNavigationController::update(const double nowSeconds)
{
  if (state_ != PlanetNavigationState::Ascending && state_ != PlanetNavigationState::Descending)
    return;
  const double elapsed = std::max(0.0, (std::isfinite(nowSeconds) ? nowSeconds : transitionStartSeconds_) - transitionStartSeconds_);
  transitionProgress_ = transitionDurationSeconds_ > 0.0 ? std::clamp(elapsed / transitionDurationSeconds_, 0.0, 1.0) : 1.0;
  pose_ = interpolatePlanetNavigationPose(transitionStartPose_, transitionTargetPose_, transitionProgress_);
  const double eased = transitionProgress_ * transitionProgress_ * (3.0 - 2.0 * transitionProgress_);
  cameraUp_ = cameraUpForDirection(
      pose_.direction,
      {transitionStartCameraUp_.x + (transitionTargetCameraUp_.x - transitionStartCameraUp_.x) * eased,
       transitionStartCameraUp_.y + (transitionTargetCameraUp_.y - transitionStartCameraUp_.y) * eased,
       transitionStartCameraUp_.z + (transitionTargetCameraUp_.z - transitionStartCameraUp_.z) * eased});
  if (transitionProgress_ < 1.0)
    return;

  if (state_ == PlanetNavigationState::Ascending)
  {
    state_ = PlanetNavigationState::Orbit;
    pose_ = orbitPose_;
    cameraUp_ = orbitCameraUp_;
  }
  else
  {
    state_ = PlanetNavigationState::Surface;
    pose_ = surfacePose_;
    cameraUp_ = surfaceCameraUp_;
    surfaceRestorePending_ = true;
  }
}

bool PlanetNavigationController::orbitInputEnabled() const
{
  return state_ == PlanetNavigationState::Ascending ||
      state_ == PlanetNavigationState::Orbit ||
      state_ == PlanetNavigationState::LoadingTarget;
}

bool PlanetNavigationController::applyOrbitDrag(const double deltaPixelsX, const double deltaPixelsY)
{
  if (!orbitInputEnabled() || !std::isfinite(deltaPixelsX) || !std::isfinite(deltaPixelsY))
    return false;
  const double yaw = -deltaPixelsX * settings_.orbitDragRadiansPerPixel;
  const double pitch = -deltaPixelsY * settings_.orbitDragRadiansPerPixel;
  const auto rotateFrame = [yaw, pitch](
                               PlanetNavigationPose &pose,
                               procedural::SpherePoint &cameraUp)
  {
    procedural::SpherePoint direction = normalized(pose.direction);
    cameraUp = cameraUpForDirection(direction, cameraUp);
    direction = normalized(rotateAroundAxis(direction, cameraUp, yaw));
    const procedural::SpherePoint cameraRight =
        normalized(cross(cameraUp, direction));
    direction =
        normalized(rotateAroundAxis(direction, cameraRight, pitch));
    cameraUp = cameraUpForDirection(
        direction, rotateAroundAxis(cameraUp, cameraRight, pitch));
    pose.direction = direction;
  };
  rotateFrame(pose_, cameraUp_);
  if (state_ == PlanetNavigationState::Ascending)
  {
    rotateFrame(orbitPose_, orbitCameraUp_);
    rotateFrame(transitionStartPose_, transitionStartCameraUp_);
    rotateFrame(transitionTargetPose_, transitionTargetCameraUp_);
  }
  else
  {
    orbitPose_ = pose_;
    orbitCameraUp_ = cameraUp_;
  }
  return true;
}

bool PlanetNavigationController::applyOrbitZoom(const double wheelSteps)
{
  if (!orbitInputEnabled() || !std::isfinite(wheelSteps))
    return false;
  const double minimum = std::max(1.000001, settings_.minimumOrbitDistanceRadii);
  const double maximum = std::max(minimum, settings_.maximumOrbitDistanceRadii);
  if (state_ == PlanetNavigationState::Ascending)
  {
    orbitPose_.orbitDistanceRadii = std::clamp(
        orbitPose_.orbitDistanceRadii *
            std::exp(-wheelSteps * settings_.orbitZoomExponentPerStep),
        minimum, maximum);
    orbitPose_.altitudeMetres =
        planetRadiusMetres_ * (orbitPose_.orbitDistanceRadii - 1.0);
    transitionTargetPose_.orbitDistanceRadii =
        orbitPose_.orbitDistanceRadii;
    transitionTargetPose_.altitudeMetres = orbitPose_.altitudeMetres;
    return true;
  }
  pose_.orbitDistanceRadii = std::clamp(pose_.orbitDistanceRadii * std::exp(-wheelSteps * settings_.orbitZoomExponentPerStep), minimum, maximum);
  pose_.altitudeMetres = planetRadiusMetres_ * (pose_.orbitDistanceRadii - 1.0);
  orbitPose_ = pose_;
  return true;
}

bool PlanetNavigationController::beginOrbitPointer(const double cursorPixelsX, const double cursorPixelsY)
{
  cancelOrbitPointer();
  if (!orbitInputEnabled() || !std::isfinite(cursorPixelsX) || !std::isfinite(cursorPixelsY))
    return false;
  orbitPointerActive_ = true;
  orbitPointerStart_ = {cursorPixelsX, cursorPixelsY};
  orbitPointerLast_ = orbitPointerStart_;
  return true;
}

bool PlanetNavigationController::updateOrbitPointer(const double cursorPixelsX, const double cursorPixelsY)
{
  if (!orbitPointerActive_ || !orbitInputEnabled() || !std::isfinite(cursorPixelsX) || !std::isfinite(cursorPixelsY))
    return false;
  const double startX = cursorPixelsX - orbitPointerStart_[0];
  const double startY = cursorPixelsY - orbitPointerStart_[1];
  const double threshold = std::max(0.0, settings_.clickDragThresholdPixels);
  if (!orbitPointerDragging_ && startX * startX + startY * startY > threshold * threshold)
  {
    orbitPointerDragging_ = true;
    applyOrbitDrag(startX, startY);
  }
  else if (orbitPointerDragging_)
    applyOrbitDrag(cursorPixelsX - orbitPointerLast_[0], cursorPixelsY - orbitPointerLast_[1]);
  orbitPointerLast_ = {cursorPixelsX, cursorPixelsY};
  return true;
}

std::optional<std::array<double, 2>> PlanetNavigationController::endOrbitPointer(const double cursorPixelsX, const double cursorPixelsY)
{
  if (!orbitPointerActive_)
    return std::nullopt;
  updateOrbitPointer(cursorPixelsX, cursorPixelsY);
  const bool click = orbitInputEnabled() && !orbitPointerDragging_;
  const std::array<double, 2> cursor{cursorPixelsX, cursorPixelsY};
  cancelOrbitPointer();
  return click ? std::optional<std::array<double, 2>>(cursor) : std::nullopt;
}

void PlanetNavigationController::cancelOrbitPointer()
{
  orbitPointerActive_ = false;
  orbitPointerDragging_ = false;
  orbitPointerStart_ = {};
  orbitPointerLast_ = {};
}

double PlanetNavigationController::overviewBlend() const
{
  if (state_ == PlanetNavigationState::Surface)
    return 0.0;
  if (state_ == PlanetNavigationState::Orbit || state_ == PlanetNavigationState::LoadingTarget)
    return 1.0;
  const double start = std::max(1.0, settings_.overviewBlendStartRadii);
  const double end = std::max(start + 1.0e-6, settings_.overviewBlendEndRadii);
  const double t = std::clamp((pose_.orbitDistanceRadii - start) / (end - start), 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

double PlanetNavigationController::localRenderWeight() const
{
  return 1.0 - overviewBlend();
}

bool PlanetNavigationController::voxelRenderingEnabled() const
{
  return localRenderWeight() > 0.01;
}

bool PlanetNavigationController::voxelEffectsEnabled() const
{
  return localRenderWeight() > 0.25;
}

PlanetTangentFrame PlanetNavigationController::overviewCameraFrame() const
{
  PlanetTangentFrame frame{};
  frame.up = normalized(pose_.direction);
  frame.north = cameraUpForDirection(frame.up, cameraUp_);
  frame.east = normalized(cross(frame.north, frame.up));
  frame.north = normalized(cross(frame.up, frame.east));
  return frame;
}

PlanetSurfaceCameraPose PlanetNavigationController::localCameraPose() const
{
  PlanetSurfaceCameraPose camera = savedSurfaceCamera_;
  const double relativeAltitude = std::clamp(pose_.altitudeMetres - surfacePose_.altitudeMetres, 0.0, std::max(settings_.maximumLocalAltitudeMetres, 0.0));
  camera.positionMetres[1] += relativeAltitude;
  camera.orientation = pose_.orientation;
  return camera;
}

bool PlanetNavigationController::beginLoadingTarget()
{
  if (state_ == PlanetNavigationState::LoadingTarget)
    return true;
  if (state_ != PlanetNavigationState::Orbit)
    return false;
  state_ = PlanetNavigationState::LoadingTarget;
  return true;
}

bool PlanetNavigationController::cancelPendingTarget()
{
  if (state_ != PlanetNavigationState::LoadingTarget)
    return false;
  state_ = PlanetNavigationState::Orbit;
  return true;
}

bool PlanetNavigationController::completeLoadingTarget(
    const PlanetNavigationPose &targetSurfacePose,
    const PlanetSurfaceCameraPose &targetSurfaceCamera,
    const double nowSeconds)
{
  if (state_ != PlanetNavigationState::LoadingTarget)
    return false;
  surfacePose_ = sanitizedPose(targetSurfacePose);
  savedSurfaceCamera_ = targetSurfaceCamera;
  savedSurfaceCamera_.orientation = surfacePose_.orientation;
  surfaceCameraUp_ = cameraUpForDirection(surfacePose_.direction, cameraUp_);
  beginTransition(
      PlanetNavigationState::Descending, surfacePose_, surfaceCameraUp_,
      settings_.descentDurationSeconds, nowSeconds);
  return true;
}

std::optional<PlanetSurfaceCameraPose> PlanetNavigationController::consumeSurfaceRestorePose()
{
  if (!surfaceRestorePending_)
    return std::nullopt;
  surfaceRestorePending_ = false;
  return savedSurfaceCamera_;
}

} // namespace voxel::svraster
