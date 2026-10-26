#include "rendering/animation/Skeleton.hpp"

#include <cmath>
#include <stdexcept>

namespace rendering
{
namespace animation
{

void Skeleton::setDefaultTransform(const math::Mat4f &transform)
{
  defaultTransform_ = transform;
}

const math::Mat4f &Skeleton::getDefaultTransform() const
{
  return defaultTransform_;
}

uint32_t Skeleton::addBone(const Bone &bone)
{
  bones_.push_back(bone);
  return static_cast<uint32_t>(bones_.size() - 1u);
}

void Skeleton::clear()
{
  defaultTransform_ = math::Mat4f::identity();
  bones_.clear();
  meshPartToBoneIndex_.clear();
}

bool Skeleton::empty() const
{
  return bones_.empty();
}

uint32_t Skeleton::getBoneCount() const
{
  return static_cast<uint32_t>(bones_.size());
}

Skeleton::Bone &Skeleton::getBone(uint32_t boneIndex)
{
  if (boneIndex >= bones_.size())
    throw std::out_of_range("Skeleton::getBone index out of range");
  return bones_[boneIndex];
}

const Skeleton::Bone &Skeleton::getBone(uint32_t boneIndex) const
{
  if (boneIndex >= bones_.size())
    throw std::out_of_range("Skeleton::getBone index out of range");
  return bones_[boneIndex];
}

const std::vector<Skeleton::Bone> &Skeleton::getBones() const
{
  return bones_;
}

void Skeleton::setMeshPartToBoneIndex(std::vector<uint32_t> mapping)
{
  meshPartToBoneIndex_ = std::move(mapping);
}

const std::vector<uint32_t> &Skeleton::getMeshPartToBoneIndex() const
{
  return meshPartToBoneIndex_;
}

std::vector<math::Mat4f> Skeleton::buildRestPoseWorldTransforms() const
{
  std::vector<math::Mat4f> worldTransforms(bones_.size(), math::Mat4f::identity());
  for (size_t boneIndex = 0; boneIndex < bones_.size(); ++boneIndex)
  {
    const Bone &bone = bones_[boneIndex];
    if (bone.parentIndex >= 0)
      worldTransforms[boneIndex] = worldTransforms[static_cast<size_t>(bone.parentIndex)] * bone.defaultLocalTransform;
    else
      worldTransforms[boneIndex] = defaultTransform_ * bone.defaultLocalTransform;
  }
  return worldTransforms;
}

std::vector<math::Mat4f> Skeleton::buildRestPoseSkinningPalette() const
{
  std::vector<math::Mat4f> palette(bones_.size(), math::Mat4f::identity());
  if (bones_.empty())
    return palette;

  const std::vector<math::Mat4f> worldTransforms = buildRestPoseWorldTransforms();
  const math::Mat4f inverseSkeletonTransform = defaultTransform_.inverse();
  for (size_t boneIndex = 0; boneIndex < bones_.size(); ++boneIndex)
    palette[boneIndex] = worldTransforms[boneIndex] * bones_[boneIndex].inverseBindMatrix * inverseSkeletonTransform;
  return palette;
}

std::vector<math::Mat4f> Skeleton::buildIdentitySkinningPalette() const
{
  return buildRestPoseSkinningPalette();
}

} // namespace animation
} // namespace rendering
