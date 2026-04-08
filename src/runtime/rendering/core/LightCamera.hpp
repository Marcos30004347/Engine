#pragma once

#include "math/math.hpp"

namespace rendering
{

// Builds view + projection matrices for a light source directly from
// position/target, without the Euler-angle roundtrip used by Camera.
// Supports both perspective (spot/point) and orthographic (directional) lights.
class LightCamera
{
public:
  struct PerspectiveDesc
  {
    float fovY;
    float aspect;
    float nearPlane;
    float farPlane;
    bool reverseZ;
  };

  struct OrthographicDesc
  {
    float left;
    float right;
    float bottom;
    float top;
    float nearPlane;
    float farPlane;
    bool reverseZ;
  };

  LightCamera() = default;

  static LightCamera perspective(const math::Vec3f &position, const math::Vec3f &target, const PerspectiveDesc &desc,
                                 const math::Vec3f &up = math::Vec3f(0.0f, 1.0f, 0.0f))
  {
    LightCamera lc;
    lc.position_ = position;
    lc.viewMatrix_ = math::Mat4f::lookAt(position, target, up);
    lc.projMatrix_ = math::Mat4f::perspective(desc.fovY, desc.aspect, desc.nearPlane, desc.farPlane, desc.reverseZ);
    return lc;
  }

  static LightCamera orthographic(const math::Vec3f &position, const math::Vec3f &target, const OrthographicDesc &desc,
                                  const math::Vec3f &up = math::Vec3f(0.0f, 1.0f, 0.0f))
  {
    LightCamera lc;
    lc.position_ = position;
    lc.viewMatrix_ = math::Mat4f::lookAt(position, target, up);
    lc.projMatrix_ = math::Mat4f::orthographic(desc.left, desc.right, desc.bottom, desc.top, desc.nearPlane, desc.farPlane, desc.reverseZ);
    return lc;
  }

  const math::Mat4f &getViewMatrix() const { return viewMatrix_; }
  const math::Mat4f &getProjectionMatrix() const { return projMatrix_; }
  const math::Vec3f &getPosition() const { return position_; }

private:
  math::Mat4f viewMatrix_;
  math::Mat4f projMatrix_;
  math::Vec3f position_;
};

} // namespace rendering
