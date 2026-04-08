#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "math/math.hpp"

namespace rendering
{
namespace animation
{

class Skeleton
{
public:
  struct Bone
  {
    std::string name;
    int32_t parentIndex = -1;
    math::Mat4f defaultLocalTransform = math::Mat4f::identity();
    math::Mat4f inverseBindMatrix = math::Mat4f::identity();
  };

  Skeleton() = default;

  void setDefaultTransform(const math::Mat4f &transform);
  const math::Mat4f &getDefaultTransform() const;

  uint32_t addBone(const Bone &bone);
  void clear();

  bool empty() const;
  uint32_t getBoneCount() const;

  Bone &getBone(uint32_t boneIndex);
  const Bone &getBone(uint32_t boneIndex) const;
  const std::vector<Bone> &getBones() const;

  void setMeshPartToBoneIndex(std::vector<uint32_t> mapping);
  const std::vector<uint32_t> &getMeshPartToBoneIndex() const;

  std::vector<math::Mat4f> buildRestPoseWorldTransforms() const;
  std::vector<math::Mat4f> buildRestPoseSkinningPalette() const;
  std::vector<math::Mat4f> buildIdentitySkinningPalette() const;

private:
  math::Mat4f defaultTransform_ = math::Mat4f::identity();
  std::vector<Bone> bones_;
  std::vector<uint32_t> meshPartToBoneIndex_;
};

} // namespace animation
} // namespace rendering
