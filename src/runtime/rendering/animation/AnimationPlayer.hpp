#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "rendering/animation/AnimationFile.hpp"
#include "rendering/animation/Skeleton.hpp"

namespace rendering
{
namespace animation
{

class AnimationPlayer
{
public:
  AnimationPlayer() = default;
  explicit AnimationPlayer(const Skeleton &skeleton);

  void setSkeleton(const Skeleton &skeleton);
  const Skeleton &getSkeleton() const;

  void clearAnimations();
  bool addAnimation(AnimationFile animation);
  bool loadAnimation(const std::string &path);

  const AnimationFile *findAnimation(const std::string &name) const;
  const std::vector<AnimationFile> &getAnimations() const;

  void resetToBindPose();
  bool applyAnimationFrame(const AnimationFile &animation, float timeSeconds, bool looping = true);
  bool applyAnimationFrame(const std::string &animationName, float timeSeconds, bool looping = true);

  const std::vector<math::Mat4f> &getLocalTransforms() const;
  const std::vector<math::Mat4f> &getWorldTransforms() const;
  const std::vector<math::Mat4f> &getBoneTransforms() const;

private:
  void rebuildBindPoseCache();
  void rebuildPose(
      const std::vector<math::Vec3f> &translations,
      const std::vector<math::Quatf> &rotations,
      const std::vector<math::Vec3f> &scales);

  Skeleton skeleton_;
  std::vector<AnimationFile> animations_;
  std::unordered_map<std::string, size_t> animationNameToIndex_;

  std::vector<math::Vec3f> bindTranslations_;
  std::vector<math::Quatf> bindRotations_;
  std::vector<math::Vec3f> bindScales_;

  std::vector<math::Mat4f> localTransforms_;
  std::vector<math::Mat4f> worldTransforms_;
  std::vector<math::Mat4f> boneTransforms_;
};

} // namespace animation
} // namespace rendering
