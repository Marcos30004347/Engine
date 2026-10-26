#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "math/math.hpp"

namespace rendering
{
namespace animation
{

class Animation
{
public:
  struct Channel
  {
    uint32_t boneIndex = 0u;
    std::vector<float> keyframeTimes;
    std::vector<math::Mat4f> keyframeTransforms;
  };

  std::string name;
  float durationSeconds = 0.0f;
  std::vector<Channel> channels;

  bool save(const std::string &path) const;
  static bool load(const std::string &path, Animation &outAnimation);
};

} // namespace animation
} // namespace rendering
