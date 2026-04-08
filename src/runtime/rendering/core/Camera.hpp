#pragma once

#include "math/math.hpp"

namespace rendering
{
class Camera
{
public:
  Camera();
  Camera(float fov, const math::Vec3f &position, const math::Vec3f &forward, bool reverseZ);

  void setAspectRatio(float aspect);
  void setNearFar(float near, float far);

  void setPosition(const math::Vec3f &position);
  void setRotation(const math::Quatf &rotation);
  void lookAt(const math::Vec3f &target, const math::Vec3f &up = math::Vec3f(0, 1, 0));

  const math::Mat4f &getViewMatrix() const;
  const math::Mat4f &getProjectionMatrix() const;
  math::Mat4f getInverseViewMatrix() const;
  math::Mat4f getInverseProjectionMatrix() const;
  const math::Vec3f &getPosition() const;
  const math::Quatf &getRotation() const;
  bool isReverseZ() const;

  math::Vec3f getForward() const;
  math::Vec3f getRight() const;
  math::Vec3f getUp() const;

  void updateMatrices();

private:
  float fov_;
  float aspectRatio_;
  float nearPlane_;
  float farPlane_;
  bool reverseZ_;

  math::Vec3f position_;
  math::Quatf rotation_;

  math::Mat4f viewMatrix_;
  math::Mat4f projectionMatrix_;

  bool viewDirty_;
  bool projectionDirty_;
};
} // namespace rendering
