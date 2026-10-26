#include "voxel/svraster/SparseVoxelTree64TLASBridge.hpp"

#include <cmath>

namespace voxel::svraster
{
namespace
{
// Column-major, so translation is elements 12..14 and the upper 3x3 is the
// first three columns.
bool isIdentityRotation(const std::array<float, 16> &m)
{
  constexpr float tolerance = 1.0e-5f;
  const std::array<float, 9> expected{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
  for (uint32_t column = 0u; column < 3u; ++column)
    for (uint32_t row = 0u; row < 3u; ++row)
      if (std::abs(m[column * 4u + row] - expected[column * 3u + row]) > tolerance)
        return false;
  // A projective row would make the negated-translation inverse wrong even with
  // an identity basis.
  return std::abs(m[3]) <= tolerance && std::abs(m[7]) <= tolerance &&
      std::abs(m[11]) <= tolerance && std::abs(m[15] - 1.0f) <= tolerance;
}
} // namespace

bool sparseVoxelTree64TransformIsTranslationOnly(
    const std::array<float, 16> &worldToModel)
{
  return isIdentityRotation(worldToModel);
}

std::vector<softwarebvh::SoftwareTLASManager::InstanceBuildInput>
sparseVoxelTree64TLASInstances(
    const std::vector<SparseVoxelTree64DynamicInstanceSource> &instances)
{
  std::vector<softwarebvh::SoftwareTLASManager::InstanceBuildInput> inputs;
  inputs.reserve(instances.size());
  for (const auto &instance : instances)
  {
    softwarebvh::SoftwareTLASManager::InstanceBuildInput input{};
    input.instanceId = instance.stableInstanceId;
    // A BLAS handle addresses a Tree64 model, not a triangle mesh.
    input.blasHandle = instance.modelId;
    input.flags = instance.terrainGeneration != 0u
        ? softwarebvh::SoftwareTLASManager::INSTANCE_FLAG_DYNAMIC
        : 0u;

    math::Mat4f objectToWorld = math::Mat4f::identity();
    if (sparseVoxelTree64TransformIsTranslationOnly(instance.worldToModel))
    {
      // Exact, and the common case: every terrain page is placed this way.
      objectToWorld.data[12] = -instance.worldToModel[12];
      objectToWorld.data[13] = -instance.worldToModel[13];
      objectToWorld.data[14] = -instance.worldToModel[14];
    }
    else
    {
      math::Mat4f worldToModel{};
      for (uint32_t element = 0u; element < 16u; ++element)
        worldToModel.data[element] = instance.worldToModel[element];
      objectToWorld = worldToModel.inverse();
    }
    input.transform = objectToWorld;
    input.previousTransform = objectToWorld;

    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      input.worldBounds.minPoint[axis] = instance.worldBounds.minimum[axis];
      input.worldBounds.maxPoint[axis] = instance.worldBounds.maximum[axis];
    }
    inputs.push_back(std::move(input));
  }
  return inputs;
}

std::vector<uint32_t> sparseVoxelTree64TLASInstanceSlots(
    const std::vector<SparseVoxelTree64DynamicInstanceSource> &instances,
    const std::vector<uint32_t> &instanceIndexToInputIndex,
    const bool stableTerrainSlots)
{
  constexpr uint32_t unresolved = 0xffffffffu;
  std::vector<uint32_t> slots(instanceIndexToInputIndex.size(), unresolved);
  for (uint32_t tlasIndex = 0u; tlasIndex < instanceIndexToInputIndex.size(); ++tlasIndex)
  {
    const uint32_t inputIndex = instanceIndexToInputIndex[tlasIndex];
    if (inputIndex >= instances.size())
      continue;
    slots[tlasIndex] = stableTerrainSlots
        ? instances[inputIndex].stableInstanceId
        : inputIndex;
  }
  return slots;
}

bool sparseVoxelTree64ValidateTLASInstanceSlots(
    const std::vector<SparseVoxelTree64DynamicInstanceSource> &instances,
    const std::vector<uint32_t> &instanceIndexToInputIndex,
    const std::vector<uint32_t> &slots,
    const bool stableTerrainSlots,
    std::string *error)
{
  constexpr uint32_t unresolved = 0xffffffffu;
  const auto reject = [&](const char *message)
  {
    if (error != nullptr)
      *error = message;
    return false;
  };
  if (slots.size() != instanceIndexToInputIndex.size())
    return reject("Tree64 TLAS slot table does not match TLAS instance count");
  for (uint32_t tlasIndex = 0u; tlasIndex < slots.size(); ++tlasIndex)
  {
    const uint32_t inputIndex = instanceIndexToInputIndex[tlasIndex];
    if (inputIndex >= instances.size())
    {
      if (slots[tlasIndex] != unresolved)
        return reject("Tree64 TLAS slot table resolved an invalid instance");
      continue;
    }
    const auto &instance = instances[inputIndex];
    const uint32_t expectedSlot = stableTerrainSlots
        ? instance.stableInstanceId
        : inputIndex;
    if (slots[tlasIndex] != expectedSlot)
      return reject("Tree64 TLAS slot table points at the wrong dynamic instance");
    if (instance.terrainGeneration != 0u &&
        (instance.terrainSlot != instance.stableInstanceId ||
         instance.modelId != instance.stableInstanceId))
      return reject("Tree64 TLAS terrain instance is not slot-stable");
  }
  return true;
}

} // namespace voxel::svraster
