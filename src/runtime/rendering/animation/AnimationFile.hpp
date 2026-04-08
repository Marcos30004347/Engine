#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "math/math.hpp"
#include "rendering/animation/Skeleton.hpp"

namespace rendering
{
namespace animation
{

class AnimationFile
{
public:
  enum class Interpolation : uint32_t
  {
    Step = 0u,
    Linear = 1u,
  };

  struct Vec3Track
  {
    Interpolation interpolation = Interpolation::Linear;
    std::vector<float> times;
    std::vector<math::Vec3f> values;
  };

  struct QuatTrack
  {
    Interpolation interpolation = Interpolation::Linear;
    std::vector<float> times;
    std::vector<math::Quatf> values;
  };

  struct BoneAnimation
  {
    uint32_t boneIndex = UINT32_MAX;
    bool hasTranslation = false;
    bool hasRotation = false;
    bool hasScale = false;
    Vec3Track translation;
    QuatTrack rotation;
    Vec3Track scale;
  };

  std::string name;
  float durationSeconds = 0.0f;
  std::vector<BoneAnimation> boneAnimations;

  bool save(const std::string &path) const;
  static bool load(const std::string &path, AnimationFile &outAnimation);

  static bool createFromGLTF(const std::string &path, const Skeleton &skeleton, std::vector<AnimationFile> &outAnimations);
};

} // namespace animation
} // namespace rendering
