#include "Camera.hpp"

using namespace rendering;
using namespace math;

Camera::Camera()
    : fov_(math::radians(60.0f)), aspectRatio_(16.0f / 9.0f), nearPlane_(0.1f), farPlane_(1000.0f), reverseZ_(false), position_(Vec3f(0.0f, 0.0f, 0.0f)),
      rotation_(Quatf::identity()), viewMatrix_(), projectionMatrix_(), viewDirty_(true), projectionDirty_(true)
{
  updateMatrices();
}

Camera::Camera(float fov, const Vec3f &position, const Vec3f &forward, bool reverseZ)
    : fov_(fov), aspectRatio_(16.0f / 9.0f), nearPlane_(0.1f), farPlane_(1000.0f), reverseZ_(reverseZ), position_(position), viewMatrix_(), projectionMatrix_(), viewDirty_(true),
      projectionDirty_(true)
{
  Vec3f forwardNorm = forward.normalize();
  Vec3f right = Vec3f(0, 1, 0).cross(forwardNorm).normalize();
  Vec3f up = forwardNorm.cross(right).normalize();

  float pitch = std::asin(-forwardNorm.y());
  float yaw = std::atan2(forwardNorm.x(), forwardNorm.z());

  rotation_ = Quatf::fromEuler(pitch, yaw, 0.0f);

  updateMatrices();
}

void Camera::setAspectRatio(float aspect)
{
  aspectRatio_ = aspect;
  projectionDirty_ = true;
}

void Camera::setNearFar(float near, float far)
{
  nearPlane_ = near;
  farPlane_ = far;
  projectionDirty_ = true;
}

void Camera::setPosition(const Vec3f &position)
{
  position_ = position;
  viewDirty_ = true;
}

void Camera::setRotation(const Quatf &rotation)
{
  rotation_ = rotation.normalize();
  viewDirty_ = true;
}

void Camera::lookAt(const Vec3f &target, const Vec3f &up)
{
  Vec3f forward = (target - position_).normalize();
  Vec3f right = forward.cross(up).normalize();
  Vec3f newUp = right.cross(forward).normalize();

  float pitch = std::asin(-forward.y());
  float yaw = std::atan2(forward.x(), forward.z());

  rotation_ = Quatf::fromEuler(pitch, yaw, 0.0f);
  viewDirty_ = true;
}

const Mat4f &Camera::getViewMatrix() const
{
  return viewMatrix_;
}

const Mat4f &Camera::getProjectionMatrix() const
{
  return projectionMatrix_;
}

Mat4f Camera::getInverseViewMatrix() const
{
  return viewMatrix_.inverse();
}

Mat4f Camera::getInverseProjectionMatrix() const
{
  return projectionMatrix_.inverse();
}

const Vec3f &Camera::getPosition() const
{
  return position_;
}

const Quatf &Camera::getRotation() const
{
  return rotation_;
}

bool Camera::isReverseZ() const
{
  return reverseZ_;
}

Vec3f Camera::getForward() const
{
  return rotation_ * Vec3f(0, 0, -1);
}

Vec3f Camera::getRight() const
{
  return rotation_ * Vec3f(1, 0, 0);
}

Vec3f Camera::getUp() const
{
  return rotation_ * Vec3f(0, 1, 0);
}

void Camera::updateMatrices()
{
  if (viewDirty_)
  {
    Vec3f forward = getForward();
    Vec3f target = position_ + forward;
    viewMatrix_ = Mat4f::lookAt(position_, target, Vec3f(0, 1, 0));
    viewDirty_ = false;
  }

  if (projectionDirty_)
  {
    projectionMatrix_ = Mat4f::perspective(fov_, aspectRatio_, nearPlane_, farPlane_, reverseZ_);
    projectionDirty_ = false;
  }
}
