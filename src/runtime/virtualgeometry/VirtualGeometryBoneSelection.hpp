#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

#include "rendering/animation/Skeleton.hpp"

namespace virtualgeometry::detail
{

constexpr float kBoneDepthPenaltyPerLevel = 0.75f;
constexpr float kAncestorSupportScale = 1.0f;

inline bool isValidBoneIndex(const rendering::animation::Skeleton *skeleton, uint32_t boneIndex)
{
  return skeleton != nullptr && boneIndex < skeleton->getBoneCount();
}

struct BoneSelectionContext
{
  const rendering::animation::Skeleton *skeleton = nullptr;
  uint32_t selectionLevel = 0u;
  std::vector<uint32_t> boneDepths;
  uint32_t maxBoneDepth = 0u;

  BoneSelectionContext(const rendering::animation::Skeleton *inSkeleton, uint32_t inSelectionLevel)
      : skeleton(inSkeleton), selectionLevel(inSelectionLevel)
  {
    if (skeleton == nullptr || skeleton->empty())
      return;

    boneDepths.resize(skeleton->getBoneCount(), 0u);
    for (uint32_t boneIndex = 0u; boneIndex < skeleton->getBoneCount(); ++boneIndex)
    {
      uint32_t depth = 0u;
      int32_t current = skeleton->getBone(boneIndex).parentIndex;
      while (current >= 0 && static_cast<uint32_t>(depth) < skeleton->getBoneCount())
      {
        ++depth;
        current = skeleton->getBone(static_cast<uint32_t>(current)).parentIndex;
      }

      boneDepths[boneIndex] = depth;
      maxBoneDepth = std::max(maxBoneDepth, depth);
    }
  }

  float directPriorityMultiplier(uint32_t boneIndex) const
  {
    if (selectionLevel == 0u || !isValidBoneIndex(skeleton, boneIndex) || maxBoneDepth == 0u)
      return 1.0f;

    const float depthNorm = static_cast<float>(boneDepths[boneIndex]) / static_cast<float>(maxBoneDepth);
    return 1.0f / (1.0f + static_cast<float>(selectionLevel) * kBoneDepthPenaltyPerLevel * depthNorm);
  }

  float ancestorPriorityMultiplier(uint32_t boneIndex, uint32_t ancestorDistance) const
  {
    if (selectionLevel == 0u)
      return 0.0f;

    return kAncestorSupportScale * directPriorityMultiplier(boneIndex) *
           (static_cast<float>(selectionLevel) / static_cast<float>(selectionLevel + std::max(ancestorDistance, 1u)));
  }
};

inline bool isAllowedCandidateBone(const std::vector<uint32_t> *candidateBones, uint32_t boneIndex)
{
  if (candidateBones == nullptr || candidateBones->empty())
    return true;
  return std::binary_search(candidateBones->begin(), candidateBones->end(), boneIndex);
}

inline void accumulateBoneScore(
    std::unordered_map<uint32_t, float> &scores,
    const BoneSelectionContext &context,
    const std::vector<uint32_t> *candidateBones,
    uint32_t boneIndex,
    float weight)
{
  if (weight <= 0.0f || boneIndex == UINT32_MAX)
    return;

  if (isAllowedCandidateBone(candidateBones, boneIndex))
    scores[boneIndex] += weight * context.directPriorityMultiplier(boneIndex);

  if (!isValidBoneIndex(context.skeleton, boneIndex))
    return;

  int32_t parentIndex = context.skeleton->getBone(boneIndex).parentIndex;
  uint32_t ancestorDistance = 1u;
  while (parentIndex >= 0)
  {
    const uint32_t parentBoneIndex = static_cast<uint32_t>(parentIndex);
    if (isAllowedCandidateBone(candidateBones, parentBoneIndex))
      scores[parentBoneIndex] += weight * context.ancestorPriorityMultiplier(parentBoneIndex, ancestorDistance);

    if (!isValidBoneIndex(context.skeleton, parentBoneIndex))
      break;

    parentIndex = context.skeleton->getBone(parentBoneIndex).parentIndex;
    ++ancestorDistance;
  }
}

inline uint32_t pickHighestScoringBone(const std::unordered_map<uint32_t, float> &scores)
{
  float bestScore = std::numeric_limits<float>::lowest();
  uint32_t bestBoneIndex = UINT32_MAX;
  for (const auto &entry : scores)
  {
    if (entry.second > bestScore || (entry.second == bestScore && entry.first < bestBoneIndex))
    {
      bestScore = entry.second;
      bestBoneIndex = entry.first;
    }
  }

  return bestBoneIndex;
}

} // namespace virtualgeometry::detail
