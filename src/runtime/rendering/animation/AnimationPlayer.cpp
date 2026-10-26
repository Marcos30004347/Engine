#include "rendering/animation/AnimationPlayer.hpp"

#include <algorithm>
#include <cmath>

namespace rendering
{
namespace animation
{

namespace
{

math::Mat4f composeTransform(const math::Vec3f &translation, const math::Quatf &rotation, const math::Vec3f &scale)
{
  return math::Mat4f::translate(translation) * math::Mat4f::fromQuaternion(rotation.normalize()) * math::Mat4f::scale(scale);
}

math::Quatf quaternionFromRotationMatrix(const math::Mat4f &matrix)
{
  const float m00 = matrix.at(0, 0);
  const float m11 = matrix.at(1, 1);
  const float m22 = matrix.at(2, 2);
  const float trace = m00 + m11 + m22;

  if (trace > 0.0f)
  {
    const float s = std::sqrt(trace + 1.0f) * 2.0f;
    return math::Quatf(
        (matrix.at(2, 1) - matrix.at(1, 2)) / s,
        (matrix.at(0, 2) - matrix.at(2, 0)) / s,
        (matrix.at(1, 0) - matrix.at(0, 1)) / s,
        0.25f * s)
        .normalize();
  }

  if (m00 > m11 && m00 > m22)
  {
    const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
    return math::Quatf(
        0.25f * s,
        (matrix.at(0, 1) + matrix.at(1, 0)) / s,
        (matrix.at(0, 2) + matrix.at(2, 0)) / s,
        (matrix.at(2, 1) - matrix.at(1, 2)) / s)
        .normalize();
  }

  if (m11 > m22)
  {
    const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
    return math::Quatf(
        (matrix.at(0, 1) + matrix.at(1, 0)) / s,
        0.25f * s,
        (matrix.at(1, 2) + matrix.at(2, 1)) / s,
        (matrix.at(0, 2) - matrix.at(2, 0)) / s)
        .normalize();
  }

  const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
  return math::Quatf(
      (matrix.at(0, 2) + matrix.at(2, 0)) / s,
      (matrix.at(1, 2) + matrix.at(2, 1)) / s,
      0.25f * s,
      (matrix.at(1, 0) - matrix.at(0, 1)) / s)
      .normalize();
}

void decomposeTransform(const math::Mat4f &matrix, math::Vec3f &translation, math::Quatf &rotation, math::Vec3f &scale)
{
  translation = math::Vec3f(matrix.at(0, 3), matrix.at(1, 3), matrix.at(2, 3));

  math::Vec3f axisX(matrix.at(0, 0), matrix.at(1, 0), matrix.at(2, 0));
  math::Vec3f axisY(matrix.at(0, 1), matrix.at(1, 1), matrix.at(2, 1));
  math::Vec3f axisZ(matrix.at(0, 2), matrix.at(1, 2), matrix.at(2, 2));

  scale = math::Vec3f(axisX.length(), axisY.length(), axisZ.length());

  math::Mat4f rotationMatrix = math::Mat4f::identity();
  if (scale.x() > 0.0f)
  {
    rotationMatrix.at(0, 0) = axisX.x() / scale.x();
    rotationMatrix.at(1, 0) = axisX.y() / scale.x();
    rotationMatrix.at(2, 0) = axisX.z() / scale.x();
  }
  if (scale.y() > 0.0f)
  {
    rotationMatrix.at(0, 1) = axisY.x() / scale.y();
    rotationMatrix.at(1, 1) = axisY.y() / scale.y();
    rotationMatrix.at(2, 1) = axisY.z() / scale.y();
  }
  if (scale.z() > 0.0f)
  {
    rotationMatrix.at(0, 2) = axisZ.x() / scale.z();
    rotationMatrix.at(1, 2) = axisZ.y() / scale.z();
    rotationMatrix.at(2, 2) = axisZ.z() / scale.z();
  }

  rotation = quaternionFromRotationMatrix(rotationMatrix);
}

float normalizeAnimationTime(float timeSeconds, float durationSeconds, bool looping)
{
  if (durationSeconds <= 0.0f)
    return 0.0f;
  if (!looping)
    return std::max(0.0f, std::min(timeSeconds, durationSeconds));

  const float wrapped = std::fmod(timeSeconds, durationSeconds);
  return wrapped < 0.0f ? wrapped + durationSeconds : wrapped;
}

size_t findKeyframeInterval(const std::vector<float> &times, float timeSeconds)
{
  if (times.size() < 2u || timeSeconds <= times.front())
    return 0u;
  if (timeSeconds >= times.back())
    return times.size() - 2u;

  const auto upper = std::upper_bound(times.begin(), times.end(), timeSeconds);
  const size_t upperIndex = static_cast<size_t>(upper - times.begin());
  return upperIndex == 0u ? 0u : upperIndex - 1u;
}

math::Vec3f sampleVec3Track(const AnimationFile::Vec3Track &track, float timeSeconds)
{
  if (track.values.empty())
    return math::Vec3f(0.0f, 0.0f, 0.0f);
  if (track.values.size() == 1u || track.times.size() < 2u)
    return track.values.front();

  const size_t keyIndex = findKeyframeInterval(track.times, timeSeconds);
  const float startTime = track.times[keyIndex];
  const float endTime = track.times[keyIndex + 1u];
  if (track.interpolation == AnimationFile::Interpolation::Step || endTime <= startTime)
    return track.values[keyIndex];

  const float alpha = (timeSeconds - startTime) / (endTime - startTime);
  const math::Vec3f &a = track.values[keyIndex];
  const math::Vec3f &b = track.values[keyIndex + 1u];
  return a * (1.0f - alpha) + b * alpha;
}

math::Quatf sampleQuatTrack(const AnimationFile::QuatTrack &track, float timeSeconds)
{
  if (track.values.empty())
    return math::Quatf::identity();
  if (track.values.size() == 1u || track.times.size() < 2u)
    return track.values.front().normalize();

  const size_t keyIndex = findKeyframeInterval(track.times, timeSeconds);
  const float startTime = track.times[keyIndex];
  const float endTime = track.times[keyIndex + 1u];
  if (track.interpolation == AnimationFile::Interpolation::Step || endTime <= startTime)
    return track.values[keyIndex].normalize();

  math::Quatf a = track.values[keyIndex].normalize();
  math::Quatf b = track.values[keyIndex + 1u].normalize();
  float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
  if (dot < 0.0f)
  {
    dot = -dot;
    b.x = -b.x;
    b.y = -b.y;
    b.z = -b.z;
    b.w = -b.w;
  }

  const float alpha = (timeSeconds - startTime) / (endTime - startTime);
  if (dot > 0.9995f)
  {
    return math::Quatf(
               a.x + (b.x - a.x) * alpha,
               a.y + (b.y - a.y) * alpha,
               a.z + (b.z - a.z) * alpha,
               a.w + (b.w - a.w) * alpha)
        .normalize();
  }

  const float theta0 = std::acos(std::max(-1.0f, std::min(dot, 1.0f)));
  const float theta = theta0 * alpha;
  const float sinTheta = std::sin(theta);
  const float sinTheta0 = std::sin(theta0);

  const float s0 = std::cos(theta) - dot * sinTheta / sinTheta0;
  const float s1 = sinTheta / sinTheta0;

  return math::Quatf(
             a.x * s0 + b.x * s1,
             a.y * s0 + b.y * s1,
             a.z * s0 + b.z * s1,
             a.w * s0 + b.w * s1)
      .normalize();
}

} // namespace

AnimationPlayer::AnimationPlayer(const Skeleton &skeleton)
{
  setSkeleton(skeleton);
}

void AnimationPlayer::setSkeleton(const Skeleton &skeleton)
{
  skeleton_ = skeleton;
  clearAnimations();
  rebuildBindPoseCache();
  resetToBindPose();
}

const Skeleton &AnimationPlayer::getSkeleton() const
{
  return skeleton_;
}

void AnimationPlayer::clearAnimations()
{
  animations_.clear();
  animationNameToIndex_.clear();
}

bool AnimationPlayer::addAnimation(AnimationFile animation)
{
  for (const AnimationFile::BoneAnimation &boneAnimation : animation.boneAnimations)
  {
    if (boneAnimation.boneIndex >= skeleton_.getBoneCount())
      return false;
  }

  const size_t animationIndex = animations_.size();
  animationNameToIndex_[animation.name] = animationIndex;
  animations_.push_back(std::move(animation));
  return true;
}

bool AnimationPlayer::loadAnimation(const std::string &path)
{
  AnimationFile animation;
  if (!AnimationFile::load(path, animation))
    return false;
  return addAnimation(std::move(animation));
}

const AnimationFile *AnimationPlayer::findAnimation(const std::string &name) const
{
  const auto it = animationNameToIndex_.find(name);
  if (it == animationNameToIndex_.end() || it->second >= animations_.size())
    return nullptr;
  return &animations_[it->second];
}

const std::vector<AnimationFile> &AnimationPlayer::getAnimations() const
{
  return animations_;
}

void AnimationPlayer::resetToBindPose()
{
  rebuildPose(bindTranslations_, bindRotations_, bindScales_);
}

bool AnimationPlayer::applyAnimationFrame(const AnimationFile &animation, float timeSeconds, bool looping)
{
  const uint32_t boneCount = skeleton_.getBoneCount();
  if (boneCount == 0u)
  {
    localTransforms_.clear();
    worldTransforms_.clear();
    boneTransforms_.clear();
    return true;
  }

  std::vector<math::Vec3f> translations = bindTranslations_;
  std::vector<math::Quatf> rotations = bindRotations_;
  std::vector<math::Vec3f> scales = bindScales_;

  const float sampleTime = normalizeAnimationTime(timeSeconds, animation.durationSeconds, looping);
  for (const AnimationFile::BoneAnimation &boneAnimation : animation.boneAnimations)
  {
    if (boneAnimation.boneIndex >= boneCount)
      continue;

    const size_t boneIndex = static_cast<size_t>(boneAnimation.boneIndex);
    if (boneAnimation.hasTranslation)
      translations[boneIndex] = sampleVec3Track(boneAnimation.translation, sampleTime);
    if (boneAnimation.hasRotation)
      rotations[boneIndex] = sampleQuatTrack(boneAnimation.rotation, sampleTime);
    if (boneAnimation.hasScale)
      scales[boneIndex] = sampleVec3Track(boneAnimation.scale, sampleTime);
  }

  rebuildPose(translations, rotations, scales);
  return true;
}

bool AnimationPlayer::applyAnimationFrame(const std::string &animationName, float timeSeconds, bool looping)
{
  const AnimationFile *animation = findAnimation(animationName);
  if (animation == nullptr)
    return false;
  return applyAnimationFrame(*animation, timeSeconds, looping);
}

const std::vector<math::Mat4f> &AnimationPlayer::getLocalTransforms() const
{
  return localTransforms_;
}

const std::vector<math::Mat4f> &AnimationPlayer::getWorldTransforms() const
{
  return worldTransforms_;
}

const std::vector<math::Mat4f> &AnimationPlayer::getBoneTransforms() const
{
  return boneTransforms_;
}

void AnimationPlayer::rebuildBindPoseCache()
{
  const size_t boneCount = static_cast<size_t>(skeleton_.getBoneCount());
  bindTranslations_.resize(boneCount);
  bindRotations_.resize(boneCount);
  bindScales_.resize(boneCount);

  for (size_t boneIndex = 0u; boneIndex < boneCount; ++boneIndex)
    decomposeTransform(skeleton_.getBone(static_cast<uint32_t>(boneIndex)).defaultLocalTransform, bindTranslations_[boneIndex], bindRotations_[boneIndex], bindScales_[boneIndex]);
}

void AnimationPlayer::rebuildPose(
    const std::vector<math::Vec3f> &translations,
    const std::vector<math::Quatf> &rotations,
    const std::vector<math::Vec3f> &scales)
{
  const size_t boneCount = static_cast<size_t>(skeleton_.getBoneCount());
  localTransforms_.resize(boneCount);
  worldTransforms_.resize(boneCount);
  boneTransforms_.resize(boneCount);

  const math::Mat4f inverseSkeletonTransform = skeleton_.getDefaultTransform().inverse();
  for (size_t boneIndex = 0u; boneIndex < boneCount; ++boneIndex)
  {
    localTransforms_[boneIndex] = composeTransform(translations[boneIndex], rotations[boneIndex], scales[boneIndex]);

    const Skeleton::Bone &bone = skeleton_.getBone(static_cast<uint32_t>(boneIndex));
    if (bone.parentIndex >= 0)
      worldTransforms_[boneIndex] = worldTransforms_[static_cast<size_t>(bone.parentIndex)] * localTransforms_[boneIndex];
    else
      worldTransforms_[boneIndex] = skeleton_.getDefaultTransform() * localTransforms_[boneIndex];

    boneTransforms_[boneIndex] = worldTransforms_[boneIndex] * bone.inverseBindMatrix * inverseSkeletonTransform;
  }
}

} // namespace animation
} // namespace rendering
